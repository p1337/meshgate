#pragma once

#include <Arduino.h>
#include "config_ui.h"

bool wifi_net_connect_sta(const String& ssid, const String& pass);
// Connect the station interface in the background while keeping the setup AP
// and captive portal online.
void wifi_net_connect_ap_sta_async(const String& ssid, const String& pass);
void wifi_net_start_mdns(const BridgeConfig& cfg);
void wifi_net_pause_mdns_for_ota();
void wifi_net_resume_mdns_after_ota();

/**
 * @return Active advertised mDNS hostname (e.g. "mesh-af28.local") or empty if not active.
 * Thread-safe: written once during boot before the webserver starts serving requests.
 */
const String& wifi_net_get_mdns_host();

void wifi_net_start_ap();
bool wifi_net_is_ap_mode();
void wifi_net_loop();

// WiFi scanning for captive portal configuration
void wifi_net_start_scan();
bool wifi_net_is_scanning();
String wifi_net_get_scan_results_json();
