#include "ble_client.h"
#include "web_log.h"
#include "utils.h"
#include <NimBLEDevice.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include "build_options.h"

/**
 * Is Initialized Flag, gets set to true after initialization.
 */
static std::atomic<bool> isInitialized{false};

/**
 * Is scanning for BLE devices flag, gets set to true while a scan
 * is active.
 */
static std::atomic<bool> isScanningFlag{false};

/**
 * Contains the last error that occurred during a BLE device
 * discovery scan, or gets set to empty string if no such error
 * is present.
 *
 * NOTE: As currently written, should be thread safe for reads/writes.
 */
static String lastScanError = "";

#ifdef ESP_IDF5_MIGRATION_BUILD
class BridgeScanCallbacks : public NimBLEScanCallbacks {
public:
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        isScanningFlag = false;
        if (reason != 0) {
            lastScanError = "BLE scan ended with error " + String(reason) + ".";
            log_w("[BLE] Scan ended with reason %d after finding %d device(s).", reason, results.getCount());
            return;
        }
        log_i("[BLE] Scan finished. Found %d device(s).", results.getCount());
    }
};

static BridgeScanCallbacks bridgeScanCallbacks;
#endif

// Meshtastic's official BLE service UUID. The legacy UUID is retained for
// radios running older firmware. Filtering advertisements by these UUIDs keeps
// unrelated rings, watches, trackers, and anonymous BLE beacons out of the UI.
static const NimBLEUUID MESHTASTIC_SERVICE_UUID("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
static const NimBLEUUID MESHTASTIC_LEGACY_SERVICE_UUID("cb0b9a0b-a8c2-49c0-bdd5-3fa12b04d84b");

/**
 * Initialize bluetooth radio.
 */
void ble_client_init() {
    // Prevent multiple initializations.
    if (isInitialized) {
        return;
    }
    NimBLEDevice::init("Meshtastic-Bridge");

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);       // Active scan requests scan response packets to get device names
    pScan->setInterval(100);          // 100ms scan interval
    pScan->setWindow(99);             // 99ms scan window
    pScan->setDuplicateFilter(true);  // Filter duplicate advertisements during a single scan
#ifdef ESP_IDF5_MIGRATION_BUILD
    pScan->setScanCallbacks(&bridgeScanCallbacks, false);
#endif

    isInitialized = true;
    log_i("[BLE] Client initialized.");
}

/**
 * Start scanning for available BLE devices asynchronously.
 * Flips isScanningFlag to true.  Once the scan has completed,
 * the isScanningFlag will return to false.
 *
 * Results can be retrieved by calling ble_client_get_scan_results_json()
 * after the scan has completed.
 */
void ble_client_start_scan() {
    // Init if not already initialized.
    if (!isInitialized) {
        ble_client_init();
    }

    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (isScanningFlag || pScan->isScanning()) {
        log_i("[BLE] Scan already in progress, ignoring request.");
        return;
    }

    // Clear previously cached results before starting a fresh scan
    pScan->clearResults();
    lastScanError = "";
    isScanningFlag = true;

    // Start asynchronous scan for BLUETOOTH_SCAN_TIME_SECONDS
    log_i("[BLE] Starting scan (%d seconds)...", BLUETOOTH_SCAN_TIME_SECONDS);
    bool started =
#ifdef ESP_IDF5_MIGRATION_BUILD
        pScan->start(BLUETOOTH_SCAN_TIME_SECONDS * 1000UL, false, true);
#else
        pScan->start(BLUETOOTH_SCAN_TIME_SECONDS, [](NimBLEScanResults results) {
        isScanningFlag = false;
        log_i("[BLE] Scan finished. Found %d device(s).", results.getCount());
    }, false);
#endif

    if (!started) {
        isScanningFlag = false;
        lastScanError = "Failed to start scan.";
        log_e("[BLE] Failed to start scan.");
    }
}

/**
 * Stop the BLE device discovery scan.
 */
void ble_client_stop_scan() {
    // If not initialized, cannot be scanning.
    if (!isInitialized) {
        log_i("[BLE] Not initialized, no scan to stop.");
        return;
    }
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (isScanningFlag || pScan->isScanning()) {
        pScan->stop();
        isScanningFlag = false;
        log_i("[BLE] scan stopped.");
    } else {
        log_i("[BLE] No scan to stop.");
    }
}

/**
 * Check the status of BLE Device Discovery Scan.
 * @return true if discovery scan is active, false if not.
 */
bool ble_client_is_scanning() {
    if (!isInitialized) {
        return false;
    }
    return isScanningFlag || NimBLEDevice::getScan()->isScanning();
}

/**
 * Represents a discovered BLE device.
 */
struct DiscoveredDevice {
    // Reported device name.
    String name;

    // Reported MAC address.
    String mac;

    // RSSI Signal Strength of device.
    int rssi;
};

String ble_client_get_scan_error() {
    return lastScanError;
}

/**
 * Returns array of discovered devices, in JSON format.  This will return a
 * max of <BLUETOOTH_MAX_DEVICES_DISCOVERABLE> devices, sorted by name descending, and by RSSI (signal strength).
 *
 * @return Results of the BLE device discovery scan, in JSON format.
 *         Example:
 *         [
 *              {
 *                  "name": "device_name_here",
 *                  "mac": "AA:BB:CC:DD:EE:FF",
 *                  "rssi": 1
                },
                ...
 *         ]
 */
String ble_client_get_scan_results_json() {
    if (!isInitialized) {
        return "[]";
    }

    NimBLEScanResults results = NimBLEDevice::getScan()->getResults();
    int count = results.getCount();

    std::vector<DiscoveredDevice> devices;
    devices.reserve(count);

    for (int i = 0; i < count; i++) {
#ifdef ESP_IDF5_MIGRATION_BUILD
        const NimBLEAdvertisedDevice* devicePtr = results.getDevice(i);
        if (!devicePtr) {
            continue;
        }
        const NimBLEAdvertisedDevice& device = *devicePtr;
#else
        NimBLEAdvertisedDevice device = results.getDevice(i);
#endif
        String name = String(device.getName().c_str());
        String mac = String(device.getAddress().toString().c_str());
        int rssi = device.getRSSI();

        const bool isMeshtastic =
            device.isAdvertisingService(MESHTASTIC_SERVICE_UUID) ||
            device.isAdvertisingService(MESHTASTIC_LEGACY_SERVICE_UUID);

        // Only offer radios that advertise a Meshtastic BLE service. A radio
        // may legitimately omit its human-readable name, so the service UUID
        // (not the name) is the authoritative discriminator.
        if (!isMeshtastic) {
            continue;
        }

        if (name.isEmpty()) {
            name = "Meshtastic device";
        }

        // Skip devices with empty MAC address.
        if (mac.isEmpty()) {
            continue;
        }

        // Deduplicate by MAC
        bool exists = false;
        for (auto& d : devices) {
            if (d.mac.equalsIgnoreCase(mac)) {
                // If existing entry has no name but this advertisement has one, update it
                if (d.name.isEmpty() && !name.isEmpty()) {
                    d.name = name;
                }
                if (rssi > d.rssi) {
                    d.rssi = rssi;
                }
                exists = true;
                break;
            }
        }

        if (!exists) {
            devices.push_back({name, mac, rssi});
        }
    }

    // Sort: devices with names first, then by RSSI (strongest signal first)
    std::sort(devices.begin(), devices.end(), [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
        bool aHasName = !a.name.isEmpty();
        bool bHasName = !b.name.isEmpty();
        if (aHasName != bHasName) {
            return aHasName > bHasName;
        }
        return a.rssi > b.rssi;
    });

    // Strictly limit to the top N strongest/named devices to protect RAM
    size_t max_results = min(devices.size(), (size_t)BLUETOOTH_MAX_DEVICES_DISCOVERABLE);

    String json;
    json.reserve(max_results * 80);
    json = "[";
    for (size_t i = 0; i < max_results; i++) {
        if (i > 0) json += ",";
        json += "{\"name\":\"" + utils_escape_json(devices[i].name) + "\",";
        json += "\"mac\":\"" + devices[i].mac + "\",";
        json += "\"rssi\":" + String(devices[i].rssi) + "}";
    }
    json += "]";

    return json;
}

/**
 * Gets set to the configured BLE device PIN.
 */
static uint32_t activePasskey = 123456;

/**
 * State flag, gets set to true when testing a BLE connection/configuration.
 */
static std::atomic<bool> isTestingFlag{false};

/**
 * Cached BLE connection test result.
 */
static BleTestResult lastTestResult = {false, "", ""};

class BridgeClientCallbacks : public NimBLEClientCallbacks {
public:
    bool passkeyPrompted = false;
    bool authCompleted = false;
    bool authFailed = false;

    void reset() {
        passkeyPrompted = false;
        authCompleted = false;
        authFailed = false;
    }

#ifdef ESP_IDF5_MIGRATION_BUILD
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        passkeyPrompted = true;
        log_i("[BLE] Passkey requested by server; configured PIN provided (hidden)");
        NimBLEDevice::injectPassKey(connInfo, activePasskey);
    }

    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
        passkeyPrompted = true;
        log_i("[BLE] Confirming pairing PIN (hidden)");
        NimBLEDevice::injectConfirmPasskey(connInfo, pin == activePasskey);
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        authCompleted = true;
        authFailed = !connInfo.isEncrypted();
        log_i(
            "[BLE] Authentication complete. Encrypted: %d, Authenticated: %d, Bonded: %d\n",
            connInfo.isEncrypted(),
            connInfo.isAuthenticated(),
            connInfo.isBonded()
        );
    }
#else
    uint32_t onPassKeyRequest() override {
        passkeyPrompted = true;
        log_i("[BLE] Passkey requested by server; configured PIN provided (hidden)");
        return activePasskey;
    }

    bool onConfirmPIN(uint32_t pin) override {
        passkeyPrompted = true;
        log_i("[BLE] Confirming pairing PIN (hidden)");
        return (pin == activePasskey);
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        authCompleted = true;
        authFailed = !desc->sec_state.encrypted;
        log_i(
            "[BLE] Authentication complete. Encrypted: %d, Authenticated: %d, Bonded: %d\n",
            desc->sec_state.encrypted,
            desc->sec_state.authenticated,
            desc->sec_state.bonded
        );
    }
#endif
};

static BridgeClientCallbacks bridgeCallbacks;

/**
 * Test that we are able to connect to the given BLE device.
 * @param macStr MAC address of the device to connect to.
 * @param pinStr The device's PIN.
 * @return
 */
BleTestResult ble_client_test_connection(const String& macStr, const String& pinStr) {
    // Initialize BT radio if not yet initialized.
    if (!isInitialized) {
        ble_client_init();
    }

    // Stop any active scan first
    ble_client_stop_scan();

    // Sanity check provided mac address.
    if (macStr.isEmpty()) {
        return {false, "No MAC address provided.", ""};
    }

    activePasskey = pinStr.toInt();
    bridgeCallbacks.reset();

    log_i("[BLE Test] Attempting test connection to %s with configured PIN (hidden)...", macStr.c_str());

    // Determine the type of MAC address the BLE device is using.
    NimBLEAddress addr = utils_parse_ble_address(macStr);

    // Always delete existing bonding data so the PIN is genuinely challenged every test
    NimBLEDevice::deleteBond(addr);

    // Set security configuration for PIN authentication
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) {
        log_e("[BLE Test] ERROR: Failed to create BLE Client instance (out of memory).");
        return {false, "Failed to create BLE Client instance (out of memory).", ""};
    }

    pClient->setClientCallbacks(&bridgeCallbacks, false);

    // Set how long to wait for successful connection.
#ifdef ESP_IDF5_MIGRATION_BUILD
    pClient->setConnectTimeout(BLUETOOTH_TIMEOUT_SECONDS * 1000UL);
#else
    pClient->setConnectTimeout(BLUETOOTH_TIMEOUT_SECONDS);
#endif

    log_i("[BLE Test] Connecting to peripheral...");

    // Blocks until connected or timeout.
    bool connected = pClient->connect(addr, false);
    if (!connected) {
        log_i("[BLE Test] Connection failed or timed out.");

        // Cleanup connection state.
        NimBLEDevice::deleteClient(pClient);
        NimBLEDevice::deleteBond(addr);
        return {false, "Could not connect to BLE device. Ensure it is powered on and within range.", ""};
    }

    // Attempt to make an authenicated/secure connection using the PIN.
    log_i("[BLE Test] Connected. Requesting secure pairing...");
    pClient->secureConnection();

    // Wait up to BLUETOOTH_TIMEOUT_SECONDS for authentication handshake to complete
    unsigned long startAuth = millis();
    while (millis() - startAuth < (BLUETOOTH_TIMEOUT_SECONDS * 1000)) {
        if (!pClient->isConnected() || bridgeCallbacks.authFailed) {
            break;
        }
        if (bridgeCallbacks.authCompleted) {
            break;
        }
        delay(100);
    }

    bool isAuth = pClient->isConnected() && bridgeCallbacks.authCompleted && !bridgeCallbacks.authFailed;
    if (isAuth) {
        NimBLEConnInfo connInfo = pClient->getConnInfo();
        isAuth = connInfo.isEncrypted() || connInfo.isAuthenticated();
    }

    if (!isAuth) {
        log_e(
            "[BLE Test] Pairing/Auth rejected: connected=%d, authCompleted=%d, authFailed=%d",
            pClient->isConnected(),
            bridgeCallbacks.authCompleted,
            bridgeCallbacks.authFailed
        );
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        NimBLEDevice::deleteBond(addr);
        return {false, "Pairing rejected by Meshtastic radio. The 6-digit PIN is incorrect.", ""};
    }

    log_i(
        "[BLE Test] Auth verified (Encrypted: %d, Authenticated: %d). Discovering services...",
        pClient->getConnInfo().isEncrypted(),
        pClient->getConnInfo().isAuthenticated()
    );

    // We are connected, but does this device provide Meshtastic Service? Or is it just a random device?
    NimBLERemoteService* pSvc = pClient->getService("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
    if (!pSvc) {
        pSvc = pClient->getService("cb0b9a0b-a8c2-49c0-bdd5-3fa12b04d84b");
    }

    if (!pSvc) {
        log_i("[BLE Test] Meshtastic Service UUID not found.");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        NimBLEDevice::deleteBond(addr);
        return {false, "Connected, but device does NOT provide the Meshtastic BLE Service.", ""};
    }

    log_i("[BLE Test] Checking Meshtastic characteristics...");
    // Check for ToRadio and FromRadio characteristics
    NimBLERemoteCharacteristic* pFromRadio = pSvc->getCharacteristic("2c55e69e-4993-11ed-b878-0242ac120002");
    if (!pFromRadio) {
        pFromRadio = pSvc->getCharacteristic("e275fb98-3496-413f-9813-1b32525da4d9");
    }
    NimBLERemoteCharacteristic* pToRadio = pSvc->getCharacteristic("f75c76d2-129e-4dad-a1dd-7866124401e7");

    if (!pFromRadio || !pToRadio) {
        log_i("[BLE Test] Required ToRadio/FromRadio characteristics missing.");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        NimBLEDevice::deleteBond(addr);
        return {false, "Meshtastic service found, but required ToRadio / FromRadio characteristics are missing.", ""};
    }

    log_i("[BLE Test] Success! Disconnecting and clearing test bond.");
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    NimBLEDevice::deleteBond(addr);

    return {true, "Connected, paired, and verified Meshtastic radio service successfully!", ""};
}

struct TestTaskParams {
    String mac;
    String pin;
};

static void bleTestTask(void* parameter) {
    TestTaskParams* params = (TestTaskParams*)parameter;
    lastTestResult = ble_client_test_connection(params->mac, params->pin);
    delete params;
    isTestingFlag = false;
    vTaskDelete(NULL);
}

void ble_client_start_test(const String& macStr, const String& pinStr) {
    // If already running a test, refuse to start.
    if (isTestingFlag) {
        return;
    }

    // Flip flag to true.
    isTestingFlag = true;
    lastTestResult = {false, "Testing...", ""};

    TestTaskParams* params = new TestTaskParams{macStr, pinStr};
    xTaskCreatePinnedToCore(
        bleTestTask,
        "ble_test",
        4096,
        params,
        1,
        NULL,
        1 // Core 1
    );
}

/**
 * @return True if a BLE connection test is on-going.
 */
bool ble_client_is_testing() {
    return isTestingFlag;
}

/**
 * @return The results of the most recent BLE connection test
 */
BleTestResult ble_client_get_test_result() {
    return lastTestResult;
}
