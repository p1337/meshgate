#include "bridge.h"
#include "web_log.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <NimBLEDevice.h>
#include "status_led.h"
#include "build_options.h"
#include "utils.h"
#include "mqtt_net.h"
#include <atomic>

struct BridgePacket {
    uint8_t data[MESHTASTIC_MAX_PACKET_SIZE]; // Max Meshtastic ToRadio envelope / packet size
    size_t len;
};

/**
 * Queue for sending packets from TCP -> BLE.
 */
static StaticQueue_t tcp_to_ble_queue_struct;
static uint8_t tcp_to_ble_queue_storage[BRIDGE_QUEUE_SIZE * sizeof(BridgePacket)];
static QueueHandle_t tcp_to_ble_queue = NULL;

/**
 * Queue for sending packets from BLE -> TCP.
 */
static StaticQueue_t ble_to_tcp_queue_struct;
static uint8_t ble_to_tcp_queue_storage[BRIDGE_QUEUE_SIZE * sizeof(BridgePacket)];
static QueueHandle_t ble_to_tcp_queue = NULL;

/**
 * Queue for sending MQTT packets from MQTT broker -> BLE.
 */
static StaticQueue_t mqtt_to_ble_queue_struct;
static uint8_t mqtt_to_ble_queue_storage[MQTT_QUEUE_SIZE * sizeof(BridgePacket)];
static QueueHandle_t mqtt_to_ble_queue = NULL;

/**
 * Tracks if we drop packets from our queues (capacity full).
 */
static std::atomic<uint32_t> s_tcp_to_ble_dropped{0};
static std::atomic<uint32_t> s_ble_to_tcp_dropped{0};
static std::atomic<uint32_t> s_mqtt_to_ble_dropped{0};
static std::atomic<bool> s_ble_connected{false};

/**
 * Atomic traffic & packet integrity counters (wait-free, zero lock contention).
 */
static std::atomic<uint32_t> s_ble_rx_count{0};
static std::atomic<uint32_t> s_ble_tx_count{0};
static std::atomic<uint32_t> s_ble_invalid_count{0};

static std::atomic<uint32_t> s_tcp_rx_count{0};
static std::atomic<uint32_t> s_tcp_tx_count{0};
static std::atomic<uint32_t> s_tcp_invalid_count{0};

struct ClientContext {
    AsyncClient* client;
    std::vector<uint8_t> rx_buffer;
};

static AsyncServer* tcpServer = NULL;
static std::vector<ClientContext*> tcpClients;
static SemaphoreHandle_t tcpClientsMutex = NULL; // Mutex to protect tcpClients vector from concurrent modification
static std::atomic<size_t> connectedClientsCount{0};
static bool bridgeRunning = false;
static String targetBleName = "";
static String targetBleMac = "";
static uint8_t targetBleAddressType = BLE_ADDR_PUBLIC;
static uint32_t targetBlePin = 123456;

static NimBLEClient* bleClient = NULL;
static NimBLERemoteCharacteristic* toRadioChar = NULL;
static NimBLERemoteCharacteristic* fromRadioChar = NULL;
static NimBLERemoteCharacteristic* fromNumChar = NULL;

// TCP callbacks
static void onClientConnected(void* arg, AsyncClient* client) {
    if (connectedClientsCount.load() >= MAX_TCP_CLIENTS) {
        log_w("[Bridge] TCP Connection rejected from %s: Max clients (%d) reached.", client->remoteIP().toString().c_str(), MAX_TCP_CLIENTS);
        client->close();
        return;
    }

    // Crucial TCP connection parameters for stable bridging
    // We intentionally leave Nagle's algorithm enabled (false) so LwIP batches the tiny
    // BLE packets together. This prevents WiFi congestion and TCP retransmission timeouts.
    client->setNoDelay(false);
    client->setRxTimeout(TCP_IDLE_TIMEOUT_SECONDS); // 10 minute idle timeout

    // Create new client context to track this connection.
    ClientContext* ctx = new ClientContext();
    ctx->client = client;

    // Only update the tcpClients list after grabbing the mutex
    if (xSemaphoreTake(tcpClientsMutex, portMAX_DELAY) == pdTRUE) {
        tcpClients.push_back(ctx);
        connectedClientsCount.fetch_add(1, std::memory_order_relaxed);
        xSemaphoreGive(tcpClientsMutex);
    }

    log_i("[Bridge] TCP Client connected from %s. Total clients: %zu", client->remoteIP().toString().c_str(), connectedClientsCount.load());

    client->onData([](void* arg, AsyncClient* c, void* data, size_t len) {
        ClientContext* ctx = (ClientContext*)arg;
        uint8_t* buf = (uint8_t*)data;

        log_d("[Bridge-TCP] RX %zu bytes from %s", len, c->remoteIP().toString().c_str());

        // Protect against OOM (e.g., malicious stream or massive desync)
        if (ctx->rx_buffer.size() + len > 2048) {
            log_e("[Bridge-TCP] ERROR: RX Buffer overflow! Disconnecting client to prevent OOM.");
            s_tcp_invalid_count.fetch_add(1, std::memory_order_relaxed);
            c->close();
            return;
        }

        // Append new data to client's RX buffer
        ctx->rx_buffer.insert(ctx->rx_buffer.end(), buf, buf + len);

        // Process as many full frames as possible
        size_t processed = 0;
        while (ctx->rx_buffer.size() - processed >= 4) {
            if (ctx->rx_buffer[processed] == 0x94 && ctx->rx_buffer[processed+1] == 0xC3) {
                uint16_t payload_len = (ctx->rx_buffer[processed+2] << 8) | ctx->rx_buffer[processed+3];

                // Protect against unreasonably large lengths (e.g. invalid header masquerading as 0x94 0xC3)
                if (payload_len > 1024) {
                    log_e("[Bridge-TCP] ERROR: Invalid frame header payload length (%u bytes > 1024)", payload_len);
                    s_tcp_invalid_count.fetch_add(1, std::memory_order_relaxed);
                    processed++; // Invalid length, skip one byte and search again
                    continue;
                }

                if (ctx->rx_buffer.size() - processed >= 4 + payload_len) {
                    // Valid full frame received from client
                    log_d("[Bridge-TCP] Parsed frame: len %u", payload_len);
                    s_tcp_rx_count.fetch_add(1, std::memory_order_relaxed);
                    BridgePacket packet;
                    packet.len = payload_len;
                    if (payload_len <= sizeof(packet.data)) {
                        memcpy(packet.data, &ctx->rx_buffer[processed + 4], payload_len);
                        if (xQueueSend(tcp_to_ble_queue, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
                            log_w("[Bridge-TCP] WARNING: tcp_to_ble_queue full, dropped packet");
                            s_tcp_to_ble_dropped.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        log_e("[Bridge-TCP] ERROR: Payload too large (%u bytes)", payload_len);
                        s_tcp_invalid_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    processed += 4 + payload_len;
                } else {
                    // Incomplete frame, break and wait for next onData
                    break;
                }
            } else {
                // Invalid header byte, skip it
                processed++;
            }
        }

        // Remove processed bytes from buffer
        if (processed > 0) {
            ctx->rx_buffer.erase(ctx->rx_buffer.begin(), ctx->rx_buffer.begin() + processed);
        }
    }, ctx);

    client->onDisconnect([](void* arg, AsyncClient* c) {
        ClientContext* ctx = (ClientContext*)arg;

        // Only update the tcpClients list after grabbing the mutex
        if (xSemaphoreTake(tcpClientsMutex, portMAX_DELAY) == pdTRUE) {
            auto it = std::find(tcpClients.begin(), tcpClients.end(), ctx);
            if (it != tcpClients.end()) {
                tcpClients.erase(it);
                connectedClientsCount.fetch_sub(1, std::memory_order_relaxed);
            }
            xSemaphoreGive(tcpClientsMutex);
        }

        log_i("[Bridge] TCP Client disconnected. Total clients remaining: %zu", connectedClientsCount.load());
        delete ctx;
    }, ctx);
}

/**
 * Async flag when the Bluetooth device notifies us there is data to be read.
 */
static volatile bool pendingRadioRead = false;
static void notifyFromNum(NimBLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    pendingRadioRead = true;
}

// Custom BLE Client Callbacks to handle PIN authentication and disconnects
class BridgeRuntimeClientCallbacks : public NimBLEClientCallbacks {
public:
    std::atomic<bool> authCompleted{false};
    std::atomic<bool> authFailed{false};

    void resetAuthentication() {
        authCompleted.store(false, std::memory_order_relaxed);
        authFailed.store(false, std::memory_order_relaxed);
    }

    uint32_t onPassKeyRequest() override {
        log_i("[Bridge-BLE] Passkey requested; configured PIN provided (hidden)");
        return targetBlePin;
    }

    bool onConfirmPIN(uint32_t pin) override {
        log_i("[Bridge-BLE] Confirming pairing PIN (hidden)");
        return (pin == targetBlePin);
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        authFailed.store(!desc->sec_state.encrypted, std::memory_order_relaxed);
        authCompleted.store(true, std::memory_order_release);
        log_i(
            "[Bridge-BLE] Authentication complete. Encrypted: %d, Authenticated: %d, Bonded: %d",
            desc->sec_state.encrypted,
            desc->sec_state.authenticated,
            desc->sec_state.bonded
        );
    }

    void onDisconnect(NimBLEClient* pClient) override {
        log_w("[Bridge-BLE] BLE client disconnected.");
        s_ble_connected.store(false, std::memory_order_relaxed);
        mqtt_net_on_ble_disconnected();
    }
};

static BridgeRuntimeClientCallbacks bridgeCallbacks;

static void clearBleClient() {
    toRadioChar = nullptr;
    fromRadioChar = nullptr;
    fromNumChar = nullptr;

    if (bleClient) {
        if (bleClient->isConnected()) {
            bleClient->disconnect();
        }
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
    }
}

/**
 * Rediscover the configured Meshtastic radio by its advertised service UUID.
 * Nordic radios can use a private BLE address that changes independently of
 * the node ID, so a MAC captured during initial setup is not always permanent.
 */
static bool rediscoverMeshtasticRadio() {
    clearBleClient();

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->setDuplicateFilter(true);

    log_i("[Bridge-BLE] Scanning for current Meshtastic radio address...");
    NimBLEScanResults results = scan->start(BLUETOOTH_SCAN_TIME_SECONDS, false);

    static const NimBLEUUID serviceUuid("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
    static const NimBLEUUID legacyServiceUuid("cb0b9a0b-a8c2-49c0-bdd5-3fa12b04d84b");

    NimBLEAddress selectedAddress;
    String selectedName;
    size_t candidateCount = 0;
    bool exactNameMatch = false;

    for (int i = 0; i < results.getCount(); ++i) {
        NimBLEAdvertisedDevice device = results.getDevice(i);
        if (!device.isAdvertisingService(serviceUuid) &&
            !device.isAdvertisingService(legacyServiceUuid)) {
            continue;
        }

        ++candidateCount;
        String advertisedName(device.getName().c_str());
        if (candidateCount == 1) {
            selectedAddress = device.getAddress();
            selectedName = advertisedName;
        }

        if (!targetBleName.isEmpty() && advertisedName.equalsIgnoreCase(targetBleName)) {
            selectedAddress = device.getAddress();
            selectedName = advertisedName;
            exactNameMatch = true;
            break;
        }
    }

    scan->clearResults();

    if (candidateCount == 0) {
        log_w("[Bridge-BLE] No Meshtastic advertisement found during recovery scan");
        return false;
    }
    if (!exactNameMatch && candidateCount > 1) {
        log_w("[Bridge-BLE] Found %u Meshtastic radios but none matched configured name '%s'",
              static_cast<unsigned>(candidateCount), targetBleName.c_str());
        return false;
    }

    targetBleMac = String(selectedAddress.toString().c_str());
    targetBleAddressType = selectedAddress.getType();
    log_i("[Bridge-BLE] Rediscovered radio '%s' at %s (address type %u)",
          selectedName.c_str(), targetBleMac.c_str(), targetBleAddressType);
    return true;
}

/**
 * BLE Task (running on Core 1)
 * This is the task the manages:
 *  - reading data off of the BLE radio and pushing onto the TCP queue.
 *  - reading data off of the BLE queue and writing to the BLE radio.
 */
static void bridgeBleTask(void* parameter) {
    log_i("[Bridge-BLE] BLE Task started on Core 1");
    uint32_t failedConnectAttempts = 0;

    while (bridgeRunning) {
        /**
         * If not connected to the BLE device yet, attempt to connect.
         * Blink the status LED appropriately to indicate no BLE connection is active.
         */
        if (!bleClient || !bleClient->isConnected()) {
            s_ble_connected.store(false, std::memory_order_relaxed);
            status_led_set(LED_MED_BLINK);
            log_i("[Bridge-BLE] Attempting to connect to Meshtastic BLE device...");

            NimBLEAddress addr(targetBleMac.c_str(), targetBleAddressType);

            if (!bleClient) {
                NimBLEDevice::init("Meshtastic-Bridge");
                NimBLEDevice::setSecurityAuth(true, true, true);
                NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
                NimBLEDevice::setMTU(512);

                bleClient = NimBLEDevice::createClient();
                bleClient->setClientCallbacks(&bridgeCallbacks, false);
            }

            bleClient->setConnectTimeout(BLUETOOTH_TIMEOUT_SECONDS);
            bridgeCallbacks.resetAuthentication();
            if (bleClient->connect(addr, false)) {
                failedConnectAttempts = 0;
                log_i("[Bridge-BLE] Connected! Securing connection...");
                bleClient->secureConnection();

                // Pairing on nRF52 radios can take longer than a fixed delay.
                // Do not start encrypted GATT discovery until authentication
                // has actually completed or the connection timeout expires.
                unsigned long authStart = millis();
                while (bleClient->isConnected() &&
                       !bridgeCallbacks.authCompleted.load(std::memory_order_acquire) &&
                       millis() - authStart < (BLUETOOTH_TIMEOUT_SECONDS * 1000UL)) {
                    delay(100);
                }

                bool secured = false;
                if (bleClient->isConnected()) {
                    NimBLEConnInfo connInfo = bleClient->getConnInfo();
                    secured = (connInfo.isEncrypted() || connInfo.isAuthenticated()) &&
                              !bridgeCallbacks.authFailed.load(std::memory_order_relaxed);
                }

                if (!secured) {
                    log_w("[Bridge-BLE] Authentication did not complete securely; reconnecting");
                    s_ble_connected.store(false, std::memory_order_relaxed);
                    clearBleClient();
                    delay(BLUETOOTH_RECONNECT_DELAY_MS);
                    continue;
                }

                NimBLERemoteService* pSvc = bleClient->getService("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
                if (pSvc) {
                    toRadioChar = pSvc->getCharacteristic("f75c76d2-129e-4dad-a1dd-7866124401e7");
                    fromRadioChar = pSvc->getCharacteristic("2c55e69e-4993-11ed-b878-0242ac120002");
                    fromNumChar = pSvc->getCharacteristic("ed9da18c-a800-4f66-a670-aa7547e34453");

                    if (!bleClient->isConnected() || !toRadioChar || !fromRadioChar || !fromNumChar) {
                        log_w("[Bridge-BLE] Required Meshtastic characteristics unavailable; reconnecting");
                        s_ble_connected.store(false, std::memory_order_relaxed);
                        clearBleClient();
                        delay(BLUETOOTH_RECONNECT_DELAY_MS);
                        continue;
                    }

                    if (fromNumChar && fromNumChar->canNotify()) {
                        bool sub = fromNumChar->subscribe(true, notifyFromNum);
                        log_d("[Bridge-BLE] Subscribed to FromNum: %d", sub);
                    }
                    s_ble_connected.store(true, std::memory_order_relaxed);
                    log_i("[Bridge-BLE] BLE setup complete. Bridging active.");
                    status_led_set(LED_SOLID_ON);
                    mqtt_net_on_ble_connected();
                } else {
                    log_w("[Bridge-BLE] Meshtastic service not found!");
                    s_ble_connected.store(false, std::memory_order_relaxed);
                    bleClient->disconnect();
                }
            } else {
                s_ble_connected.store(false, std::memory_order_relaxed);
                ++failedConnectAttempts;
                log_w("[Bridge-BLE] Connection failed (attempt %u)",
                      static_cast<unsigned>(failedConnectAttempts));

                // Recreate the client after a timeout. On the first failure and
                // every third failure, rediscover private/rotating addresses.
                clearBleClient();
                if (failedConnectAttempts == 1 || (failedConnectAttempts % 3) == 0) {
                    if (rediscoverMeshtasticRadio()) {
                        delay(250);
                        continue;
                    }
                }

                log_w("[Bridge-BLE] Retrying in %d ms...", BLUETOOTH_RECONNECT_DELAY_MS);
                delay(BLUETOOTH_RECONNECT_DELAY_MS);
            }
        } else {
            BridgePacket packet;
            // If we have a pending read, do not sleep at all (0 ticks).
            // Otherwise, wait up to 10ms for incoming TCP packets to keep the loop fast.
            TickType_t waitTicks = pendingRadioRead ? 0 : pdMS_TO_TICKS(10);
            bool hasPacket = false;

            // High Priority: App / TCP commands and direct chat messages are checked first.
            if (xQueueReceive(tcp_to_ble_queue, &packet, waitTicks) == pdTRUE) {
                hasPacket = true;
            }
            // Normal Priority: Background MQTT downlink packets are checked
            else if (xQueueReceive(mqtt_to_ble_queue, &packet, 0) == pdTRUE) {
                hasPacket = true;
            }

            if (hasPacket) {
                log_d("[Bridge-BLE] Writing %zu bytes to ToRadio...", packet.len);
                if (toRadioChar && toRadioChar->canWrite()) {
                    // Meshtastic ToRadio expects Write Without Response (false)
                    bool success = toRadioChar->writeValue(packet.data, packet.len, false);
                    if (success) {
                        log_d("[Bridge-BLE] Write success");
                        s_ble_tx_count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        log_e("[Bridge-BLE] ERROR: Failed to write packet to ToRadio characteristic");
                        s_ble_invalid_count.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    log_e("[Bridge-BLE] ERROR: ToRadio characteristic unavailable or unwritable");
                    s_ble_invalid_count.fetch_add(1, std::memory_order_relaxed);
                }
            }

            static unsigned long lastFailsafe = 0;
            /**
             * If we have no connected clients, and aren't running a mqtt broker proxy,
             * then we can skip processing messages from the connected bluetooth device.
             *
             * If we have a connected client, we should push messages as they come in to them.
             * If we are running a mqtt broker proxy, we need to process messages for it.
             */
            if (fromRadioChar && (connectedClientsCount.load() > 0 || mqtt_net_is_enabled())) {
                // Read if notified, or if it's been 250ms since last check (failsafe)
                bool doRead = pendingRadioRead || (millis() - lastFailsafe > 250);

                if (doRead) {
                    bool wasNotified = pendingRadioRead; // Capture this before clearing it!
                    pendingRadioRead = false;
                    lastFailsafe = millis();

                    static std::string lastPacket = "";
                    static unsigned long lastPacketTime = 0;
                    std::string currentVal = fromRadioChar->readValue();

                    // Accept the packet if it's different, OR if the radio explicitly notified us it was new,
                    // OR if 2.5 seconds have passed (failsafe for dropped identical heartbeat notifications)
                    if (currentVal.length() > 0 && (currentVal != lastPacket || wasNotified || (millis() - lastPacketTime > 2500))) {
                        lastPacket = currentVal;
                        lastPacketTime = millis();

                        log_d("[Bridge-BLE] Fast-Polled %d new bytes from FromRadio!", currentVal.length());
                        s_ble_rx_count.fetch_add(1, std::memory_order_relaxed);

                        /*
                         * Pass the raw packet to the MQTT subsystem to determine if its relevant to it or not.
                         * This is used to:
                         *   - Discover and auto-syncs ModuleConfig.mqtt settings from the radio.
                         *   - Publishes MqttClientProxyMessage uplink packets to the active MQTT broker.
                         *
                         *  Returns true if it consumed it, and doesn't need further processing here.
                         *  Returns false if it needs to be processed here further.
                         */
                        bool consumedByMqtt = mqtt_net_handle_from_radio((const uint8_t*)currentVal.data(), currentVal.length());

                        /*
                         * Only forward the packet to the TCP queue if:
                         *  - It was NOT an internal MQTT proxy message (prevents echoing raw MQTT packets to apps over the tcp socket).
                         *  - There is at least one active TCP client connected (prevents filling the queue).
                         */
                        if (!consumedByMqtt && connectedClientsCount.load() > 0) {
                            BridgePacket rx_packet;
                            rx_packet.len = currentVal.length();
                            if (rx_packet.len <= sizeof(rx_packet.data)) {
                                memcpy(rx_packet.data, currentVal.data(), rx_packet.len);
                                // Apply backpressure: Wait up to 50ms if the queue is full so we don't drop packets!
                                if (xQueueSend(ble_to_tcp_queue, &rx_packet, pdMS_TO_TICKS(50)) != pdTRUE) {
                                    log_e("[Bridge-BLE] CRITICAL: ble_to_tcp_queue FULL! Dropped %zu bytes", rx_packet.len);
                                    s_ble_to_tcp_dropped.fetch_add(1, std::memory_order_relaxed);
                                }
                            } else {
                                log_e("[Bridge-BLE] ERROR: Radio Payload too large (%zu bytes)", rx_packet.len);
                                s_ble_invalid_count.fetch_add(1, std::memory_order_relaxed);
                            }
                        }

                        // We successfully pulled a NEW packet! There might be more packets
                        // waiting in the queue. Flag it to instantly read again on the next loop!
                        pendingRadioRead = true;
                    }
                }
            }

            // Always yield at least 1ms to allow equal-priority tasks on Core 1 (like Arduino loopTask) to run
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    vTaskDelete(NULL);
}

/**
 * Network broadcast task (runs on Core 0)
 * Handles:
 *  - reading data from TCP Queue and writing to connection TCP client(s).
 *  - reading data from connected TCP clients and writing to BLE queue.
 */
static void bridgeNetTask(void* parameter) {
    log_i("[Bridge-Net] Network broadcasting Task started on Core 0");

    while (bridgeRunning) {
        BridgePacket packet;
        if (xQueueReceive(ble_to_tcp_queue, &packet, pdMS_TO_TICKS(100)) == pdTRUE) {
            log_d("[Bridge-Net] Broadcasting %zu bytes to %zu TCP client", packet.len, connectedClientsCount.load());
            // Build contiguous TCP frame to prevent fragmentation desyncs
            size_t frame_len = packet.len + 4;
            uint8_t frame[MESHTASTIC_MAX_PACKET_SIZE + 4]; // Max packet size + 4-byte header

            if (frame_len <= sizeof(frame)) {
                frame[0] = 0x94;
                frame[1] = 0xC3;
                frame[2] = (uint8_t)(packet.len >> 8);
                frame[3] = (uint8_t)(packet.len & 0xFF);
                memcpy(&frame[4], packet.data, packet.len);

                // Send to all connected clients under mutex protection!
                if (xSemaphoreTake(tcpClientsMutex, portMAX_DELAY) == pdTRUE) {
                    for (ClientContext* ctx : tcpClients) {
                        if (ctx->client->space() >= frame_len) {
                            ctx->client->write((const char*)frame, frame_len);
                            ctx->client->send(); // Force immediate transmission
                            s_tcp_tx_count.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            log_e("[Bridge-Net] ERROR: Client TX buffer full! Dropped %zu bytes for %s", packet.len, ctx->client->remoteIP().toString().c_str());
                            s_tcp_invalid_count.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    xSemaphoreGive(tcpClientsMutex);
                }
            } else {
                log_e("[Bridge-Net] ERROR: Packet too large for frame buffer (%zu bytes)", packet.len);
                s_tcp_invalid_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    vTaskDelete(NULL);
}

/**
 * Enqueue an MQTT downlink packet to be transmitted to the BLE radio.
 *
 * When a MQTT message comes from the Broker, we queue it up to be sent
 * to the BLE radio.
 *
 * @return bool true on successful queueing, false if unable to queue.
 */
bool bridge_enqueue_mqtt_to_ble(const uint8_t* data, size_t len) {
    if (!mqtt_to_ble_queue || !data || len == 0) {
        return false;
    }
    if (len > sizeof(BridgePacket::data)) {
        log_e("[Bridge-MQTT] ERROR: Downlink packet too large (%zu bytes, max %zu)", len, sizeof(BridgePacket::data));
        mqtt_net_record_invalid();
        return false;
    }
    BridgePacket packet;
    packet.len = len;
    memcpy(packet.data, data, len);
    if (xQueueSend(mqtt_to_ble_queue, &packet, 0) != pdTRUE) {
        log_e("[Bridge-MQTT] ERROR: mqtt_to_ble_queue full (%d packets), dropped downlink packet", MQTT_QUEUE_SIZE);
        s_mqtt_to_ble_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

/**
 * Clear all pending packets in the MQTT downlink queue.
 */
void bridge_clear_mqtt_to_ble_queue() {
    if (mqtt_to_ble_queue) {
        xQueueReset(mqtt_to_ble_queue);
        log_i("[Bridge-MQTT] Cleared mqtt_to_ble_queue.");
    }
}

/**
 * Initialize Bridge.
 * @param ble_name Advertised BLE name used to disambiguate recovery scans.
 * @param ble_mac Last known MAC address of BLE device to connect to.
 * @param ble_pin PIN for BLE device.
 */
void bridge_init(const String& ble_name, const String& ble_mac, uint32_t ble_pin) {
    targetBleName = ble_name;
    targetBleMac = ble_mac;
    targetBleAddressType = utils_parse_ble_address(ble_mac).getType();
    targetBlePin = ble_pin;

    /**
     * Queues for passing packets between BLE <--> TCP.
     * Statically allocate FreeRTOS queues in .bss segment (zero heap fragmentation)
     * Holds up to BRIDGE_QUEUE_SIZE (defaults 100) "packets" of data
     */
    tcp_to_ble_queue = xQueueCreateStatic(
        BRIDGE_QUEUE_SIZE,
        sizeof(BridgePacket),
        tcp_to_ble_queue_storage,
        &tcp_to_ble_queue_struct
    );
    ble_to_tcp_queue = xQueueCreateStatic(
        BRIDGE_QUEUE_SIZE,
        sizeof(BridgePacket),
        ble_to_tcp_queue_storage,
        &ble_to_tcp_queue_struct
    );

    /**
     * Queue for passing packets from MQTT broker -> BLE.
     */
    mqtt_to_ble_queue = xQueueCreateStatic(
        MQTT_QUEUE_SIZE,
        sizeof(BridgePacket),
        mqtt_to_ble_queue_storage,
        &mqtt_to_ble_queue_struct
    );

    // Register MQTT Downlink Callback to enqueue ToRadio packets for BLE transmission
    mqtt_net_set_downlink_callback([](const uint8_t* to_radio_buf, size_t len) {
        bridge_enqueue_mqtt_to_ble(to_radio_buf, len);
    });

    // Mutex for thread safe modifying/reading of connected TCP clients.
    tcpClientsMutex = xSemaphoreCreateMutex();

    // Start the tcp server
    tcpServer = new AsyncServer(TCP_PORT);
    tcpServer->onClient(&onClientConnected, tcpServer);
}

static TaskHandle_t bridgeBleTaskHandle = NULL;
static TaskHandle_t bridgeNetTaskHandle = NULL;

void bridge_start() {
    // If already running, refuse to start (again).
    if (bridgeRunning) {
        return;
    }
    bridgeRunning = true;

    tcpServer->begin();
    log_i("[Bridge] TCP Server started on port %d", TCP_PORT);

    // Starts the BLE task and pins to CPU core 1
    xTaskCreatePinnedToCore(bridgeBleTask, "bridge_ble", 8192, NULL, 1, &bridgeBleTaskHandle, 1);

    // Starts the TCP/Net task and pins to CPU core 0
    xTaskCreatePinnedToCore(bridgeNetTask, "bridge_net", 4096, NULL, 1, &bridgeNetTaskHandle, 0);
}

/**
 * @return True if the BLE device is connected, false if not (wait-free atomic read)
 */
bool bridge_is_ble_connected() {
    return s_ble_connected.load(std::memory_order_relaxed);
}

/**
 * @return Stats about the running tasks, queues, etc...
 */
BridgeDiagStats bridge_get_diag_stats() {
    BridgeDiagStats stats = {};
    stats.tcp_to_ble_waiting = tcp_to_ble_queue ? uxQueueMessagesWaiting(tcp_to_ble_queue) : 0;
    stats.tcp_to_ble_capacity = BRIDGE_QUEUE_SIZE;
    stats.tcp_to_ble_dropped = s_tcp_to_ble_dropped.load(std::memory_order_relaxed);
    stats.ble_to_tcp_waiting = ble_to_tcp_queue ? uxQueueMessagesWaiting(ble_to_tcp_queue) : 0;
    stats.ble_to_tcp_capacity = BRIDGE_QUEUE_SIZE;
    stats.ble_to_tcp_dropped = s_ble_to_tcp_dropped.load(std::memory_order_relaxed);
    stats.mqtt_to_ble_waiting = mqtt_to_ble_queue ? uxQueueMessagesWaiting(mqtt_to_ble_queue) : 0;
    stats.mqtt_to_ble_capacity = MQTT_QUEUE_SIZE;
    stats.mqtt_to_ble_dropped = s_mqtt_to_ble_dropped.load(std::memory_order_relaxed);
    stats.connected_tcp_clients = connectedClientsCount.load();
    stats.ble_task_stack_free_bytes = bridgeBleTaskHandle ? (uxTaskGetStackHighWaterMark(bridgeBleTaskHandle) * sizeof(StackType_t)) : 0;
    stats.net_task_stack_free_bytes = bridgeNetTaskHandle ? (uxTaskGetStackHighWaterMark(bridgeNetTaskHandle) * sizeof(StackType_t)) : 0;
    stats.ble_connected = bridge_is_ble_connected();
    stats.ble_rx_count = s_ble_rx_count.load(std::memory_order_relaxed);
    stats.ble_tx_count = s_ble_tx_count.load(std::memory_order_relaxed);
    stats.ble_invalid_count = s_ble_invalid_count.load(std::memory_order_relaxed);
    stats.tcp_rx_count = s_tcp_rx_count.load(std::memory_order_relaxed);
    stats.tcp_tx_count = s_tcp_tx_count.load(std::memory_order_relaxed);
    stats.tcp_invalid_count = s_tcp_invalid_count.load(std::memory_order_relaxed);
    return stats;
}
