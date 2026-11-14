#include "wifi_net.h"
#include "web_log.h"
#include "config_ui.h"
#include "bridge.h"
#include "utils.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include "build_options.h"
#include "status_led.h"
#include "ip_led.h"

/**
 * DNS server used during Captive Portal Mode to ensure redirects
 * to our captive portal website.
 */
DNSServer dnsServer;

/**
 * Flag set to true when running in Captive Portal mode.
 */
bool isApMode = false;

/**
 * Holds our cached Wifi search/scan results.
 */
static String cachedWifiResultsJson = "[]";

/**
 * Holds the active advertised mDNS hostname.
 */
static String s_mdns_host = "";
static String s_mdns_base_host = "";
static String s_mdns_name = "";
static String s_mdns_short_name = "";
static String s_mdns_node_id = "";
static std::atomic<bool> s_mdns_active{false};
static std::atomic<bool> s_mdns_paused_for_ota{false};

static std::atomic<bool> s_ap_sta_connecting{false};

static bool startCachedMdnsAdvertisement() {
    if (s_mdns_base_host.isEmpty()) return false;
    if (!MDNS.begin(s_mdns_base_host.c_str())) return false;
    MDNS.setInstanceName(s_mdns_name.c_str());
    MDNS.addService("meshtastic", "tcp", TCP_PORT);
    MDNS.addServiceTxt("meshtastic", "tcp", "name", s_mdns_name.c_str());
    MDNS.addServiceTxt("meshtastic", "tcp", "shortname", s_mdns_short_name.c_str());
    MDNS.addServiceTxt("meshtastic", "tcp", "id", s_mdns_node_id.c_str());
    if (s_mdns_host.isEmpty()) s_mdns_host = s_mdns_base_host + ".local";
    s_mdns_active.store(true, std::memory_order_release);
    return true;
}

struct WifiConnectRequest {
    String ssid;
    String pass;
};

static void wifiApStaConnectTask(void* parameter) {
    WifiConnectRequest request = *static_cast<WifiConnectRequest*>(parameter);
    delete static_cast<WifiConnectRequest*>(parameter);

    log_i("[WIFI:AP_MODE] Connecting station interface to: %s", request.ssid.c_str());
    WiFi.scanDelete();
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(request.ssid.c_str(), request.pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress stationIp = WiFi.localIP();
        log_i("[WIFI:AP_MODE] Station connected. Local IP: %s", stationIp.toString().c_str());
        log_i("[WIFI:AP_MODE] Web UI also available at: http://%s/", stationIp.toString().c_str());
        ip_led_show_once(stationIp);
    } else {
        log_e("[WIFI:AP_MODE] Station connection failed; setup AP remains available");
    }

    s_ap_sta_connecting.store(false);
    vTaskDelete(nullptr);
}

/**
 * Start a scan for available wifi networks.
 */
void wifi_net_start_scan() {
    int16_t status = WiFi.scanComplete();
    if (status == WIFI_SCAN_RUNNING) {
        return;
    }
    WiFi.scanDelete();
    cachedWifiResultsJson = "[]";
    WiFi.scanNetworks(true);
}

/**
 * Check if a wifi network scan is in progress.
 * If a network scan is complete, it will populate the results of the scan
 * into the cachedWifiResultsJson variable.
 *
 * @return True if a Wifi network scan is currently in progress, false otherwise.
 */
bool wifi_net_is_scanning() {
    /**
     * This returns one of three value types.
     * -1 (WIFI_SCAN_RUNNING) : The scan is currently active in the background and hasn't finished yet.
     * -2 (WIFI_SCAN_FAILED) : The scan failed to trigger.
     * 0 or greater : The scan has successfully completed, and the integer returned is the actual count of how many WiFi networks it found.
     */
    int16_t numNetworksFound = WiFi.scanComplete();
    if (numNetworksFound == WIFI_SCAN_RUNNING) {
        // Scan is still running, not yet completed.
        // return true to indicate still running.
        return true;
    }

    if (numNetworksFound == WIFI_SCAN_FAILED) {
        log_e("[WIFI:AP_MODE] ERROR: WiFi Scan failed.");

        // What should we do to handle this? delete the scan?
        WiFi.scanDelete();
        return false;
    }

    /**
     * If the result is 0 or greater, that means the scan completed,
     * lets collect the results and store them into cachedWifiResultsJson
     */
    if (numNetworksFound >= 0 && cachedWifiResultsJson == "[]") {
        struct ScannedWifi {
            String ssid;
            int rssi;
            bool is_open;
        };

        std::vector<ScannedWifi> networks;
        networks.reserve(numNetworksFound);

        // Iterate over found networks collecting details and generating results json.
        for (int i = 0; i < numNetworksFound; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) {
                continue;
            }

            int rssi = WiFi.RSSI(i);
            bool isOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

            bool exists = false;
            for (auto& net : networks) {
                if (net.ssid == ssid) {
                    if (rssi > net.rssi) {
                        net.rssi = rssi;
                        net.is_open = isOpen;
                    }
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                networks.push_back({ssid, rssi, isOpen});
            }
        }

        // Sort by relative strength of the network signal.
        std::sort(networks.begin(), networks.end(), [](const ScannedWifi& a, const ScannedWifi& b) {
            return a.rssi > b.rssi;
        });

        // Strictly limit to the top WIFI_MAX_NETWORKS_DISCOVERABLE (default 30) strongest networks to protect RAM
        size_t max_results = min(networks.size(), (size_t)WIFI_MAX_NETWORKS_DISCOVERABLE);

        String json;
        json.reserve(max_results * 64);
        json = "[";
        for (size_t i = 0; i < max_results; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + utils_escape_json(networks[i].ssid) + "\",";
            json += "\"rssi\":" + String(networks[i].rssi) + ",";
            json += "\"is_open\":" + String(networks[i].is_open ? "true" : "false") + "}";
        }
        json += "]";

        // Store the result into cachedWifiResultsJson
        cachedWifiResultsJson = json;

        // Delete/Cleanup the scan.
        WiFi.scanDelete();
    }

    return false;
}

/**
 * @return JSON representation of found Wifi networks
 * Format of:
 * [
 *      {"ssid": "ssid_here", "rssi": 1, "is_open": true/false},
 *      ...
 * ]
 */
String wifi_net_get_scan_results_json() {
    // Ensure any finished scan results are cached
    wifi_net_is_scanning();
    return cachedWifiResultsJson;
}

void wifi_net_start_ap() {
    status_led_set(LED_SETUP_PATTERN);
    log_i("[WIFI:AP_MODE] Starting AP Mode: Meshtastic-Bridge-Setup");
    WiFi.mode(WIFI_AP_STA);
    const bool apStarted = WiFi.softAP("Meshtastic-Bridge-Setup");
    if (!apStarted) {
        log_e("[WIFI:AP_MODE] Failed to start setup access point");
        return;
    }

    log_i("[WIFI:AP_MODE] Setup AP ready. IP: %s, MAC: %s",
          WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());

    // Setup Captive Portal DNS - Route all DNS requests to the ESP32's IP
    dnsServer.start(53, "*", WiFi.softAPIP());

    // Start the Web Server
    config_ui_start_server();

    // Flip AP Mode flag to true.
    isApMode = true;
}

// Handles connecting as a wifi client/station to configured SSID and passkey.
bool wifi_net_connect_sta(const String& ssid, const String& pass) {
    log_i("[WIFI] Connecting to WiFi: %s", ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    // Wait up to 10 seconds for connection (20 * 500 ms delay)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress stationIp = WiFi.localIP();
        log_i("[WIFI] WiFi Connected successfully!");
        log_i("[WIFI] IP Address: %s", stationIp.toString().c_str());
        log_i("[WIFI] Web UI available at: http://%s/", stationIp.toString().c_str());
        ip_led_show_once(stationIp);
        return true;
    }
    return false;
}

void wifi_net_connect_ap_sta_async(const String& ssid, const String& pass) {
    if (ssid.isEmpty()) {
        return;
    }
    if (s_ap_sta_connecting.exchange(true)) {
        log_w("[WIFI:AP_MODE] Station connection is already in progress");
        return;
    }

    auto* request = new WifiConnectRequest{ssid, pass};
    if (request == nullptr) {
        s_ap_sta_connecting.store(false);
        log_e("[WIFI:AP_MODE] Could not allocate station connection request");
        return;
    }

    if (xTaskCreatePinnedToCore(wifiApStaConnectTask, "wifi_ap_sta", 4096, request, 1, nullptr, 0) != pdPASS) {
        delete request;
        s_ap_sta_connecting.store(false);
        log_e("[WIFI:AP_MODE] Could not start station connection task");
    }
}

/**
 * Starts the mDNS (Multicast DNS) service to broadcast the bridge on the local network.
 *
 * This allows the official Meshtastic apps (iOS/Android/Web) and other network tools
 * to automatically discover the bridge as if it were a physical Meshtastic node. It
 * intelligently parses the target's BLE name to mimic its exact identity on the network.
 *
 * Under normal/ideal circumstances (e.g., target BLE name is "DSC_af28"):
 * - mDNS Hostname: "dsc-af28.local" (Resolves to the ESP32's IP address)
 * - Instance Name: "DSC_af28 Bridge" (Pretty name shown in generic Bonjour browsers)
 * - Short Name:    "DSC" (Parsed from the prefix before the underscore)
 * - Node ID:       "!af28" (Parsed from the suffix after the underscore)
 *
 * Fallback behaviors:
 * - Hostname: If BLE name is empty or invalid, falls back to "meshtastic-bridge-<MAC>.local"
 * - Short Name: If BLE name lacks an underscore, falls back to a default (e.g., "BRDG")
 * - Node ID: If BLE name lacks an underscore, falls back to "!" + the last 4 characters of the WiFi MAC
 *
 * @param cfg The active BridgeConfig containing the target BLE name and MAC.
 */
void wifi_net_start_mdns(const BridgeConfig& cfg) {
    // Name advertised
    String mdns_name = "meshtastic_bridge";
    // Hostname advertised.
    String mdns_host = "Meshtastic_Bridge";
    // Shortname advertised
    String short_name = "Meshtastic_Bridge";
    // NodeId advertised
    String node_id = "!";

    // Grab wifi mac address as fall back.
    String wifiMac = WiFi.macAddress();
    wifiMac.replace(":", "");
    while (wifiMac.length() < 4) {
        wifiMac += "x";
    }
    wifiMac = wifiMac.substring(wifiMac.length() - 4);

    if (cfg.ble_name.length() > 0) {
        mdns_name = cfg.ble_name + " Bridge";

        // Parse the BLE name (e.g., "DSC_af28")
        int underscoreIdx = cfg.ble_name.lastIndexOf('_');
        if (underscoreIdx > 0 && underscoreIdx < cfg.ble_name.length() - 1) {
            short_name = cfg.ble_name.substring(0, underscoreIdx);
            node_id += cfg.ble_name.substring(underscoreIdx + 1);
        } else {
            // Fallback if BLE name has no '_', use last 4 of mac
            node_id += wifiMac;
        }
    } else {
        node_id += wifiMac;
    }

    // Ensure nodeId is at least 4 characters
    while (node_id.length() < 4) {
        node_id += "x";
    }

    // Sanitize host to strict RFC rules (alphanumeric and hyphens only)
    String sanitizedHost = "";
    for (int i = 0; i < cfg.ble_name.length(); i++) {
        char c = cfg.ble_name[i];
        if (isalnum(c) || c == '-') {
            sanitizedHost += c;
        } else if (c == ' ' || c == '_') {
            sanitizedHost += '-';
        }
    }
    if (sanitizedHost.length() == 0) {
        sanitizedHost = "meshtastic-bridge-" + wifiMac;
    }
    // RFC 1035 enforces a 63-character limit.
    if (sanitizedHost.length() > 63) {
        sanitizedHost = sanitizedHost.substring(0, 63);
    }
    mdns_host = sanitizedHost;
    mdns_host.toLowerCase();

    s_mdns_base_host = mdns_host;
    s_mdns_name = mdns_name;
    s_mdns_short_name = short_name;
    s_mdns_node_id = node_id;
    if (startCachedMdnsAdvertisement()) {
        log_i("[MDNS] mDNS auto-discovery started (%s as '%s_%s')", s_mdns_host.c_str(), short_name.c_str(), node_id.substring(node_id.length() - 4).c_str());
    } else {
        log_e("[MDNS] Error: Failed to start MDNS advertisement.");
    }
}

void wifi_net_pause_mdns_for_ota() {
    if (!s_mdns_active.exchange(false, std::memory_order_acq_rel)) return;
    s_mdns_paused_for_ota.store(true, std::memory_order_release);
    log_i("[OTA] Pausing mDNS discovery service...");
    MDNS.end();
}

void wifi_net_resume_mdns_after_ota() {
    bool expected = true;
    if (!s_mdns_paused_for_ota.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;
    log_i("[OTA] Restoring mDNS discovery service after failed update...");
    if (!startCachedMdnsAdvertisement()) {
        log_w("[MDNS] Could not restore mDNS advertisement after OTA failure");
    }
}

/**
 * @return Active advertised mDNS hostname (e.g. "mesh-af28.local") or empty if not active.
 * Thread-safe: written once during boot before the webserver starts serving requests.
 */
const String& wifi_net_get_mdns_host() {
    return s_mdns_host;
}

/**
 * @return True if running in Access Point / Captive Portal setup mode.
 *         False if running in 'normal' bridge mode.
 */
bool wifi_net_is_ap_mode() {
    return isApMode;
}

/**
 * Handle processing DNS requests if running in AP mode.
 */
void wifi_net_loop() {
    if (isApMode) {
        // Must be called repeatedly to handle DNS requests for the captive portal
        dnsServer.processNextRequest();
    }
}
