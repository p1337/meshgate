#include "mqtt_net.h"
#include "web_log.h"
#include "bridge.h"
#include "build_options.h"
#include <mqtt_client.h>
#include <esp_crt_bundle.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"
#include "utils.h"
#include <atomic>

/**
 * Embedded Mozilla Root CA Bundle
 * Generated during build process.
 */
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

/**
 * Atomic flag for wait-free status queries in fast loops.
 * True if mqtt subsystem feature flag is enabled, false if disabled.
 * NOTE: Does NOT tell you the state of the client (connected vs disconnected etc...)
 */
static std::atomic<bool> s_mqtt_enabled{false};

/**
 * Atomic message telemetry counters (wait-free, zero lock contention).
 */
static std::atomic<uint32_t> s_msgs_published{0};
static std::atomic<uint32_t> s_msgs_received{0};
static std::atomic<uint32_t> s_msgs_invalid{0};

void mqtt_net_record_invalid() {
    s_msgs_invalid.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Callback to disable certificate validation for self-signed or insecure TLS
 */
static esp_err_t mqtt_tls_insecure_attach(void *conf) {
    if (conf) {
        mbedtls_ssl_config *ssl_conf = (mbedtls_ssl_config *)conf;
        mbedtls_ssl_conf_authmode(ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    return ESP_OK;
}

/**
 * Active Broker Session Definition for change detection.
 */
struct ActiveBrokerSession {
    String server = "";
    uint16_t port = 0;
    String user = "";
    String pass = "";
    String root = "";
    bool tls = false;
    bool tls_insecure = false;
    String custom_ca = "";
    bool is_active = false;
};


/**
 * MQTT Proxy Settings from WebUI.
 */
static MqttConfig currentConfig;

/**
 * MqttStatus contains the live telemetry properties (radio_server, radio_user, radio_root, active_server, etc.)
 * queried for /status.
 */
static MqttStatus currentStatus;

/**
 * The mqtt broker password configured on the radio is excluded from MqttStatus explicitly
 * to avoid leaking it via the telemetry end points.
 */
static String radioPassword = "";

/**
 * Details about the currently connected mqtt client.
 * Generally, this matches the values in MqttStatus, but if the user
 * updates the mqtt settings on the radio, this is how we detect that change.
 */
static ActiveBrokerSession currentSession;

/**
 * Internal State & Recursive Mutex
 */
static SemaphoreHandle_t mqttMutex = NULL;
static esp_mqtt_client_handle_t mqttClient = NULL;
static MqttDownlinkCallback downlinkCallback = nullptr;

/**
 * Bluetooth Disconnect Grace Period Tracking
 */
static std::atomic<uint32_t> s_ble_disconnected_at{0};
static std::atomic<bool> s_in_grace_period{false};

/**
 * Resets grace timer state (assumes caller holds mqttMutex / MqttLockGuard).
 */
static inline void mqtt_net_reset_grace_timer() {
    s_in_grace_period.store(false, std::memory_order_relaxed);
    s_ble_disconnected_at.store(0, std::memory_order_relaxed);
}

/**
 * RAII Lock Guard for deterministic recursive mutex ownership with optional timeout
 */
class MqttLockGuard {
public:
    MqttLockGuard(TickType_t timeout = portMAX_DELAY) {
        if (mqttMutex) {
            acquired = (xSemaphoreTakeRecursive(mqttMutex, timeout) == pdTRUE);
        }
    }
    ~MqttLockGuard() {
        if (mqttMutex && acquired) {
            xSemaphoreGiveRecursive(mqttMutex);
        }
    }
    bool is_locked() const { return acquired; }
private:
    bool acquired = false;
};

/**
 * State mapping helper.
 * @param state The state to get the string/human readable representation from
 * @return The human readable state value.
 */
static const char* mqtt_state_to_str(MqttState state) {
    switch (state) {
        case MQTT_STATE_DISABLED: return "Disabled";
        case MQTT_STATE_WAITING_RADIO_CONFIG: return "Waiting for Radio Config";
        case MQTT_STATE_CONNECTING: return "Connecting...";
        case MQTT_STATE_CONNECTED: return "Connected";
        case MQTT_STATE_DISCONNECTED: return "Disconnected";
        case MQTT_STATE_ERROR: return "Error";
        default: return "Unknown";
    }
}

// Update state and log transition
static void mqtt_net_set_state(MqttState newState, const String& errorMsg = "") {
    MqttLockGuard lock;
    currentStatus.state = newState;
    currentStatus.state_str = mqtt_state_to_str(newState);
    if (!errorMsg.isEmpty()) {
        currentStatus.last_error = errorMsg;
    } else if (newState == MQTT_STATE_CONNECTED) {
        currentStatus.last_error = "";
    }
    log_i("[MQTT] State -> %s %s", currentStatus.state_str.c_str(), errorMsg.isEmpty() ? "" : errorMsg.c_str());
}

/**
 * Parses detailed error diagnostics from an MQTT_EVENT_ERROR event.
 * Logs hex codes to serial and returns a user-friendly status message.
 */
static String mqtt_net_parse_error_details(esp_mqtt_event_handle_t event) {
    String errorMsg = "Socket error / Broker unreachable";
    if (!event || !event->error_handle) {
        return errorMsg;
    }

    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        log_e(
            "[MQTT] Transport/TLS error (esp_tls_last_esp_err: 0x%x, esp_tls_stack_err: 0x%x, cert_flags: 0x%x)",
            event->error_handle->esp_tls_last_esp_err,
            event->error_handle->esp_tls_stack_err,
            event->error_handle->esp_tls_cert_verify_flags
        );
        if (event->error_handle->esp_tls_cert_verify_flags != 0) {
            errorMsg = "TLS Certificate validation failed";
        } else if (event->error_handle->esp_tls_last_esp_err != 0) {
            errorMsg = "TLS handshake / Socket error";
        }
    } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        log_e("[MQTT] Broker refused connection (code: %d)", event->error_handle->connect_return_code);
        switch (event->error_handle->connect_return_code) {
            case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
                errorMsg = "Auth failed (Bad Username/Password)";
                break;
            case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
                errorMsg = "Rejected by broker (Not Authorized)";
                break;
            case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
                errorMsg = "Broker server unavailable";
                break;
            case MQTT_CONNECTION_REFUSE_PROTOCOL:
                errorMsg = "Broker rejected MQTT protocol version";
                break;
            case MQTT_CONNECTION_REFUSE_ID_REJECTED:
                errorMsg = "Broker rejected Client ID";
                break;
            default:
                errorMsg = "Broker connection refused";
                break;
        }
    }
    return errorMsg;
}

/**
 * Event handler for ESP-IDF MQTT Client.
 * @param event The event to process.
 * @return
 */
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        // On Connect.
        case MQTT_EVENT_CONNECTED: {
            log_i("[MQTT] Connected to broker successfully.");
            mqtt_net_set_state(MQTT_STATE_CONNECTED);

            // Subscribe to synced root topic: <root>/# (e.g., "msh/#" or "msh/JP/#" etc...)
            String root = currentStatus.active_root.isEmpty() ? "msh" : currentStatus.active_root;
            String subTopic = root.endsWith("/") ? (root + "#") : (root + "/#");
            int msg_id = esp_mqtt_client_subscribe(event->client, subTopic.c_str(), 0);
            log_i("[MQTT] Subscribed to topic '%s' (msg_id: %d)", subTopic.c_str(), msg_id);
            break;
        }

        // On Disconnect.
        case MQTT_EVENT_DISCONNECTED: {
            log_w("[MQTT] Disconnected from broker.");
            bool isEnabled = false;
            {
                MqttLockGuard lock;
                isEnabled = currentConfig.enabled;
            }
            mqtt_net_set_state(isEnabled ? MQTT_STATE_DISCONNECTED : MQTT_STATE_DISABLED);
            break;
        }

        case MQTT_EVENT_SUBSCRIBED: {
            log_d("[MQTT] Subscription confirmed (msg_id: %d)", event->msg_id);
            break;
        }

        case MQTT_EVENT_DATA: {
            // Log and increment received counter
            log_d("[MQTT] Received message on topic '%.*s' (%d bytes)", event->topic_len, event->topic, event->data_len);
            s_msgs_received.fetch_add(1, std::memory_order_relaxed);

            if (!downlinkCallback) {
                log_w("[MQTT] Received MQTT message but no downlink callback is defined!");
                break;
            }

            // Construct Meshtastic ToRadio envelope with MqttClientProxyMessage
            meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
            to_radio.which_payload_variant = meshtastic_ToRadio_mqttClientProxyMessage_tag;

            meshtastic_MqttClientProxyMessage* proxy_msg = &to_radio.mqttClientProxyMessage;

            if (event->topic_len >= (int)sizeof(proxy_msg->topic)) {
                log_e("[MQTT] ERROR: Downlink topic exceeds protobuf capacity (%d >= %zu bytes). Dropping packet.",
                      event->topic_len, sizeof(proxy_msg->topic));
                mqtt_net_record_invalid();
                break;
            }
            if ((size_t)event->data_len > sizeof(proxy_msg->payload_variant.data.bytes)) {
                log_e("[MQTT] ERROR: Downlink payload exceeds protobuf capacity (%d > %zu bytes). Dropping packet.",
                      event->data_len, sizeof(proxy_msg->payload_variant.data.bytes));
                mqtt_net_record_invalid();
                break;
            }

            size_t topicLen = event->topic_len;
            memcpy(proxy_msg->topic, event->topic, topicLen);
            proxy_msg->topic[topicLen] = '\0';
            proxy_msg->retained = (event->retain != 0);

            proxy_msg->which_payload_variant = meshtastic_MqttClientProxyMessage_data_tag;
            size_t copyLen = (size_t)event->data_len;
            memcpy(proxy_msg->payload_variant.data.bytes, event->data, copyLen);
            proxy_msg->payload_variant.data.size = copyLen;

            /*
             * Encode into stack buffer.
             * Sized to MESHTASTIC_MAX_PACKET_SIZE to comfortably fit the full ToRadio envelope:
             *   - Max payload: 512 bytes
             *   - Topic string: up to 32 bytes
             *   - Retained flag: 2 bytes
             *   - Protobuf tags & length headers: ~10 bytes
             * Total encoded ToRadio envelope can reach ~556 bytes.
             */
            uint8_t to_radio_buf[MESHTASTIC_MAX_PACKET_SIZE];
            pb_ostream_t stream = pb_ostream_from_buffer(to_radio_buf, sizeof(to_radio_buf));
            if (pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
                downlinkCallback(to_radio_buf, stream.bytes_written);
            } else {
                log_e("[MQTT] ERROR: Failed to encode ToRadio protobuf for downlink (%s)", PB_GET_ERROR(&stream));
                mqtt_net_record_invalid();
            }
            break;
        }

        // On Errors
        case MQTT_EVENT_ERROR: {
            String errorMsg = mqtt_net_parse_error_details(event);
            log_e("[MQTT] Connection error: %s", errorMsg.c_str());
            mqtt_net_set_state(MQTT_STATE_ERROR, errorMsg);
            break;
        }

        // All other event types ignored.
        default:
            break;
    }
    return ESP_OK;
}

/**
 * Safely stops and destroys the active ESP-IDF MQTT client handle.
 */
static void mqtt_net_destroy_client() {
    esp_mqtt_client_handle_t clientToDestroy = NULL;
    {
        MqttLockGuard lock;
        if (mqttClient) {
            clientToDestroy = mqttClient;
            mqttClient = NULL;
        }
    }
    if (clientToDestroy) {
        log_i("[MQTT] Stopping and destroying MQTT client...");
        esp_mqtt_client_stop(clientToDestroy);
        esp_mqtt_client_destroy(clientToDestroy);
    }
}

// Client starter (releases lock before starting MQTT task to prevent deadlocks)
static void mqtt_net_start_client(const String& server, uint16_t port, const String& user, const String& pass, bool tls, bool tls_insecure, const String& custom_ca) {
    esp_mqtt_client_config_t mqtt_cfg = {};
    esp_mqtt_client_handle_t newClient = NULL;
    {
        MqttLockGuard lock;

        /**
         * If we already have a client... destroy it first...
         */
        if (mqttClient) {
            mqtt_net_destroy_client();
        }

        // Sanity check config.
        if (server.isEmpty()) {
            log_e("[MQTT] No broker address is configured on radio, cannot start.");
            mqtt_net_set_state(MQTT_STATE_ERROR, "No broker address configured on radio");
            return;
        }

        // Copy detail into the current status entry
        currentStatus.active_server = server;
        currentStatus.active_port = port;
        currentStatus.active_tls = tls;
        currentStatus.active_root = currentStatus.radio_root.isEmpty() ? "msh" : currentStatus.radio_root;

        // Track active session for seamless reconnect comparison
        currentSession.server = server;
        currentSession.port = port;
        currentSession.user = user;
        currentSession.pass = pass;
        currentSession.root = currentStatus.active_root;
        currentSession.tls = tls;
        currentSession.tls_insecure = tls_insecure;
        currentSession.custom_ca = custom_ca;
        currentSession.is_active = true;

        // Reset the disconnect grace timer on connect.
        mqtt_net_reset_grace_timer();

        // Update state to connecting...
        mqtt_net_set_state(MQTT_STATE_CONNECTING);

        // Build out the underlying mqtt client
        mqtt_cfg.host = server.c_str();
        mqtt_cfg.port = port;
        if (!user.isEmpty()) {
            mqtt_cfg.username = user.c_str();
        }
        if (!pass.isEmpty()) {
            mqtt_cfg.password = pass.c_str();
        }
        mqtt_cfg.transport = tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;

        // 3-Tier TLS Security Configuration
        if (tls) {
            if (tls_insecure) {
                // Tier 3: Insecure bypass (self-signed, local IP, or CN mismatch)
                mqtt_cfg.crt_bundle_attach = mqtt_tls_insecure_attach;
                mqtt_cfg.skip_cert_common_name_check = true;
            } else if (!custom_ca.isEmpty()) {
                // Tier 2: Custom / Private Root CA or Server Certificate PEM
                mqtt_cfg.cert_pem = custom_ca.c_str();
                mqtt_cfg.crt_bundle_attach = NULL;
            } else {
                // Tier 1: Embedded Mozilla Root CA Bundle (Let's Encrypt, DigiCert, etc.)
                mqtt_cfg.crt_bundle_attach = arduino_esp_crt_bundle_attach;
            }
        }

        mqtt_cfg.buffer_size = MQTT_CLIENT_BUFFER_SIZE;
        mqtt_cfg.out_buffer_size = MQTT_CLIENT_BUFFER_SIZE;
        mqtt_cfg.keepalive = MQTT_CLIENT_KEEPALIVE_SECONDS;
        mqtt_cfg.reconnect_timeout_ms = MQTT_RECONNECT_TIME_MS;
        mqtt_cfg.task_prio = 2; // Keep below AsyncTCP (3) and LwIP (18) so WebUI and network sockets are never starved
        mqtt_cfg.task_stack = 8192; // 8KB stack for Nanopb encode/decode and TLS handshake safety
        mqtt_cfg.event_handle = mqtt_event_handler;

        log_i(
            "[MQTT] Starting client -> %s:%u (TLS: %s, Insecure: %s, Custom CA: %s)",
            server.c_str(),
            port,
            tls ? "Yes" : "No",
            tls_insecure ? "Yes" : "No",
            custom_ca.isEmpty() ? "No" : "Yes"
        );

        newClient = esp_mqtt_client_init(&mqtt_cfg);
        mqttClient = newClient;
    } // Lock is safely released HERE before esp_mqtt_client_start!

    if (newClient) {
        esp_err_t err = esp_mqtt_client_start(newClient);
        if (err != ESP_OK) {
            log_e("[MQTT] Failed to start MQTT client (error %d)", err);
            mqtt_net_set_state(MQTT_STATE_ERROR, "Failed to start client");
        }
    } else {
        log_e("[MQTT] Failed to initialize MQTT client struct");
        mqtt_net_set_state(MQTT_STATE_ERROR, "Init failed");
    }
}

/**
 * Stops and destroys the connected client and updates all relevant state.
 */
static void mqtt_net_stop_client() {
    {
        MqttLockGuard lock;
        currentSession.is_active = false;
        mqtt_net_reset_grace_timer();
        mqtt_net_set_state(MQTT_STATE_DISABLED);
    }
    mqtt_net_destroy_client();
}

// Starts or restarts the MQTT client using the active synced radio configuration
static void mqtt_net_start_synced_client() {
    String server;
    uint16_t port = 1883;
    String user = "";
    String pass = "";
    bool tls = false;
    bool tls_insec = false;
    String custom_ca = "";

    {
        MqttLockGuard lock;
        if (!currentConfig.enabled) {
            log_i("[MQTT] Refusing to start MQTT client, MQTT Gateway feature is disabled.");
            return;
        }
        if (!currentStatus.radio_proxy_enabled) {
            log_i("[MQTT] Refusing to start MQTT client, MQTT Proxy is disabled on radio.");
            return;
        }
        server = currentStatus.radio_server.isEmpty() ? "mqtt.meshtastic.org" : currentStatus.radio_server;
        port = currentStatus.radio_port;
        user = currentStatus.radio_user;
        pass = radioPassword;
        tls = currentStatus.radio_tls;
        tls_insec = currentConfig.tls_insecure;
        custom_ca = currentConfig.custom_ca;
    }

    mqtt_net_start_client(server, port, user, pass, tls, tls_insec, custom_ca);
}

void mqtt_net_init() {
    if (!mqttMutex) {
        mqttMutex = xSemaphoreCreateRecursiveMutex();
    }
    arduino_esp_crt_bundle_set(rootca_crt_bundle_start);
}

void mqtt_net_loop() {
    // Fast lock-free check: if not in grace period or 60s has not elapsed, return immediately.
    if (!s_in_grace_period.load(std::memory_order_relaxed) ||
        (millis() - s_ble_disconnected_at.load(std::memory_order_relaxed) < (uint32_t)MQTT_BLE_GRACE_PERIOD_SECONDS * 1000)) {
        return;
    }

    /*
     * Lock acquired only on expiration to serialize against simultaneous BLE reconnect
     */
    MqttLockGuard lock;
    if (s_in_grace_period.load(std::memory_order_relaxed) && !bridge_is_ble_connected()) {
        if (millis() - s_ble_disconnected_at.load(std::memory_order_relaxed) >= (uint32_t)MQTT_BLE_GRACE_PERIOD_SECONDS * 1000) {
            log_w("[MQTT] BLE device has been disconnected for longer than grace period (%ds). Disconnecting from MQTT broker and clearing queue.", MQTT_BLE_GRACE_PERIOD_SECONDS);
            mqtt_net_reset_grace_timer();
            mqtt_net_stop_client();
            bridge_clear_mqtt_to_ble_queue();
            mqtt_net_set_state(MQTT_STATE_WAITING_RADIO_CONFIG, "BLE disconnected (grace expired)");
        }
    }
}

/**
 * Asks the connected Meshtastic radio to send its configuration (ModuleConfig, NodeInfo, Channels)
 * by sending a ToRadio packet with a random want_config_id nonce.
 */
static void mqtt_net_request_radio_config() {
    meshtastic_ToRadio req = meshtastic_ToRadio_init_default;
    req.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    uint32_t nonce = esp_random();
    req.want_config_id = (nonce != 0) ? nonce : 1;

    uint8_t buf[16];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&stream, meshtastic_ToRadio_fields, &req)) {
        if (downlinkCallback) {
            log_i("[MQTT] Requesting radio configuration (want_config_id: %u)...", req.want_config_id);
            downlinkCallback(buf, stream.bytes_written);
        } else {
            log_w("[MQTT] Cannot request radio config: no downlink callback registered to push message to.");
        }
    } else {
        log_e("[MQTT] Failed to encode want_config_id protobuf: %s", PB_GET_ERROR(&stream));
    }
}

/**
 * Updates the MQTT Gateway configuration from the webui.
 * @param cfg The configuration to apply.
 */
void mqtt_net_apply_config(const MqttConfig& cfg) {
    mqtt_net_init();
    s_mqtt_enabled.store(cfg.enabled, std::memory_order_relaxed);
    MqttLockGuard lock;
    currentConfig = cfg;

    if (!currentConfig.enabled) {
        log_i("[MQTT] Feature disabled in configuration, shutting down mqtt client...");
        mqtt_net_stop_client();
        bridge_clear_mqtt_to_ble_queue();
        return;
    }

    // If radio config is not yet discovered or proxy is disabled on radio
    if (currentStatus.radio_server.isEmpty()) {
        log_i("[MQTT] Enabled. Waiting for radio ModuleConfig.mqtt packets over Bluetooth...");
        mqtt_net_set_state(MQTT_STATE_WAITING_RADIO_CONFIG);
        if (bridge_is_ble_connected()) {
            mqtt_net_request_radio_config();
        }
    } else if (!currentStatus.radio_proxy_enabled) {
        log_w("[MQTT] Radio configuration detected, but MQTT proxy is inactive on the radio.");
        mqtt_net_stop_client();
        bridge_clear_mqtt_to_ble_queue();
        mqtt_net_set_state(MQTT_STATE_DISABLED, "Radio Proxy Disabled (Enable 'MQTT' & 'Proxy' in App)");
    } else if (bridge_is_ble_connected()) {
        log_i(
            "[MQTT] Applying Auto-Sync MQTT configuration -> %s:%u (Root: %s)",
            currentStatus.radio_server.c_str(), currentStatus.radio_port, currentStatus.radio_root.c_str()
        );
        mqtt_net_start_synced_client();
    } else {
        log_i("[MQTT] Radio configuration ready. Waiting for Bluetooth connection before connecting to broker...");
        mqtt_net_set_state(MQTT_STATE_WAITING_RADIO_CONFIG);
    }
}

/**
 * Check state of the mqtt subsystem.
 * NOTE: only tells if its enabled/disabled, not the connected state or anything like that.
 *
 * @return Returns true if MQTT Gateway feature is enabled (atomic, wait-free fast path)
 */
bool mqtt_net_is_enabled() {
    return s_mqtt_enabled.load(std::memory_order_relaxed);
}

/**
 * Called when the BLE device is connected.
 */
void mqtt_net_on_ble_connected() {
    // Fast wait-free check: do nothing if MQTT Gateway is disabled
    if (!mqtt_net_is_enabled()) {
        return;
    }

    MqttLockGuard lock;
    mqtt_net_reset_grace_timer();

    // If we are already connected to the broker (during grace period) and session is active, keep it!
    if (mqttClient && currentSession.is_active) {
        log_i("[MQTT] Bluetooth radio reconnected within grace period! Preserving active broker session.");
    } else if (currentStatus.radio_proxy_enabled && !currentStatus.radio_server.isEmpty() && !mqttClient) {
        // Connect to broker if radio configuration is ready and proxy is enabled
        log_i(
            "[MQTT] Bluetooth radio connected! Connecting to Synced MQTT broker -> %s:%u",
            currentStatus.radio_server.c_str(), currentStatus.radio_port
        );
        mqtt_net_start_synced_client();
    }

    // Always query the radio on connect to ensure any changed settings (swapped radio, updated credentials) are detected
    mqtt_net_request_radio_config();
}

/**
 * Called when the BLE device is disconnected.
 * It starts a timer where we queue messages coming from the mqtt broker while
 * waiting for the BLE device to reconnect.
 *
 * If the queue fills, we start dropping mqtt packets.
 * If the BLE device reconnects before the timer runs out, we push those messages to the BLE device.
 * If the timer expires, we disconnect from the mqtt broker and clear the queue.
 */
void mqtt_net_on_ble_disconnected() {
    MqttLockGuard lock;
    if (!currentConfig.enabled || !mqttClient) {
        return;
    }
    log_w("[MQTT] BLE disconnected. Starting %d-second grace timer...", MQTT_BLE_GRACE_PERIOD_SECONDS);
    s_ble_disconnected_at.store(millis(), std::memory_order_relaxed);
    s_in_grace_period.store(true, std::memory_order_relaxed);
}

/**
 * Sets the callback fired when a mqtt message comes in.
 * @param cb The callback to register
 */
void mqtt_net_set_downlink_callback(MqttDownlinkCallback cb) {
    MqttLockGuard lock;
    downlinkCallback = cb;
}

bool mqtt_net_publish(const char* topic, const uint8_t* payload, size_t len, bool retained) {
    MqttLockGuard lock;
    if (!mqttClient) {
        log_w("[MQTT] Unable to publish message, no mqttClient!");
        return false;
    }

    int msg_id = esp_mqtt_client_publish(mqttClient, topic, (const char*)payload, len, 0, retained ? 1 : 0);
    if (msg_id >= 0) {
        s_msgs_published.fetch_add(1, std::memory_order_relaxed);
        log_d("[MQTT] Published %zu bytes to '%s' (retained: %d)", len, topic, retained);
        return true;
    } else {
        log_e("[MQTT] Failed to publish message to topic '%s'", topic);
        return false;
    }
}

bool mqtt_net_handle_from_radio(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return false;
    }

    // Decode FromRadio protobuf using Nanopb
    meshtastic_FromRadio radio_msg = meshtastic_FromRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data, len);

    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &radio_msg)) {
        // Not a valid FromRadio packet or decoding failed
        return false;
    }

    // 1. Check for ModuleConfig.mqtt packets (Auto-Sync from Radio)
    if (radio_msg.which_payload_variant == meshtastic_FromRadio_moduleConfig_tag) {
        if (radio_msg.moduleConfig.which_payload_variant == meshtastic_ModuleConfig_mqtt_tag) {
            const meshtastic_ModuleConfig_MQTTConfig& mqtt_cfg = radio_msg.moduleConfig.payload_variant.mqtt;
            log_i(
                "[MQTT] Discovered ModuleConfig.mqtt from Radio: enabled=%d, proxy_enabled=%d, server='%s', root='%s', tls=%d",
                mqtt_cfg.enabled,
                mqtt_cfg.proxy_to_client_enabled,
                mqtt_cfg.address,
                mqtt_cfg.root,
                mqtt_cfg.tls_enabled
            );

            bool shouldStart = false;
            {
                MqttLockGuard lock;
                String rawAddress = String(mqtt_cfg.address);
                currentStatus.radio_proxy_enabled = (mqtt_cfg.enabled && mqtt_cfg.proxy_to_client_enabled);
                currentStatus.radio_root = String(mqtt_cfg.root).isEmpty() ? "msh" : String(mqtt_cfg.root);
                currentStatus.radio_tls = mqtt_cfg.tls_enabled;
                currentStatus.radio_user = String(mqtt_cfg.username);
                radioPassword = String(mqtt_cfg.password);

                String server = rawAddress.isEmpty() ? "mqtt.meshtastic.org" : rawAddress;
                uint16_t port = currentStatus.radio_tls ? 8883 : 1883;

                // If the radio address explicitly specifies a port (e.g. "160.16.104.222:1883" or "broker.local:8883")
                int colonIdx = server.indexOf(':');
                if (colonIdx > 0) {
                    String portStr = server.substring(colonIdx + 1);
                    server = server.substring(0, colonIdx);
                    int parsedPort = portStr.toInt();
                    if (parsedPort > 0 && parsedPort <= 65535) {
                        port = (uint16_t)parsedPort;
                    }
                }

                currentStatus.radio_server = server;
                currentStatus.radio_port = port;

                if (!currentStatus.radio_proxy_enabled) {
                    const char* reason = !mqtt_cfg.enabled ? "MQTT Disabled on Radio (Enable 'MQTT' in App)" : "Radio Proxy Disabled (Enable 'Proxy to Client' in App)";
                    log_w("[MQTT] Radio MQTT proxy is inactive (%s). Stopping client.", reason);
                    mqtt_net_stop_client();
                    bridge_clear_mqtt_to_ble_queue();
                    mqtt_net_set_state(MQTT_STATE_DISABLED, reason);
                    return false;
                }

                if (currentConfig.enabled && bridge_is_ble_connected()) {
                    currentStatus.active_root = currentStatus.radio_root;

                    bool configChanged = (!currentSession.is_active ||
                                          currentSession.server != currentStatus.radio_server ||
                                          currentSession.port != currentStatus.radio_port ||
                                          currentSession.user != currentStatus.radio_user ||
                                          currentSession.pass != radioPassword ||
                                          currentSession.root != currentStatus.active_root ||
                                          currentSession.tls != currentStatus.radio_tls ||
                                          currentSession.tls_insecure != currentConfig.tls_insecure ||
                                          currentSession.custom_ca != currentConfig.custom_ca);

                    if (configChanged || !mqttClient) {
                        if (currentSession.is_active) {
                            log_i("[MQTT] Radio configuration changed while connected. Reconnecting broker...");
                            bridge_clear_mqtt_to_ble_queue();
                        }
                        shouldStart = true;
                    } else {
                        log_d("[MQTT] Received identical radio ModuleConfig.mqtt - maintaining existing session.");
                    }
                }
            }

            if (shouldStart) {
                mqtt_net_start_synced_client();
            }
        }
    }

    // 2. Check for MqttClientProxyMessage (Uplink from Radio to MQTT)
    if (radio_msg.which_payload_variant == meshtastic_FromRadio_mqttClientProxyMessage_tag) {
        const meshtastic_MqttClientProxyMessage& proxy_msg = radio_msg.mqttClientProxyMessage;
        log_d("[MQTT] Intercepted MqttClientProxyMessage for topic '%s'", proxy_msg.topic);

        if (s_mqtt_enabled.load(std::memory_order_relaxed)) {
            if (proxy_msg.which_payload_variant == meshtastic_MqttClientProxyMessage_data_tag) {
                mqtt_net_publish(proxy_msg.topic, proxy_msg.payload_variant.data.bytes, proxy_msg.payload_variant.data.size, proxy_msg.retained);
            } else if (proxy_msg.which_payload_variant == meshtastic_MqttClientProxyMessage_text_tag) {
                size_t textLen = strlen(proxy_msg.payload_variant.text);
                mqtt_net_publish(proxy_msg.topic, (const uint8_t*)proxy_msg.payload_variant.text, textLen, proxy_msg.retained);
            }
            // Always consume when MQTT gateway is enabled so phone apps never receive raw proxy messages
            return true;
        }
    }

    return false;
}

MqttStatus mqtt_net_get_status() {
    MqttStatus st;
    {
        MqttLockGuard lock(pdMS_TO_TICKS(100));
        if (lock.is_locked()) {
            st = currentStatus;
        }
    }
    st.msgs_published = s_msgs_published.load(std::memory_order_relaxed);
    st.msgs_received = s_msgs_received.load(std::memory_order_relaxed);
    st.msgs_invalid = s_msgs_invalid.load(std::memory_order_relaxed);
    return st;
}
