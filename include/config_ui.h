#pragma once

#include <Arduino.h>

struct BridgeConfig {
    // WiFi settings
    String wifi_ssid;
    String wifi_pass;

    // Bluetooth settings
    String ble_name;
    String ble_mac;
    String ble_pin;

    // System settings
    bool debug_logs;
    bool ota_beta_enabled;
    bool ota_auto_update_enabled;

    // WireGuard client settings. The private and pre-shared keys are stored in
    // NVS and are never returned by the configuration HTTP API.
    bool wireguard_enabled;
    String wireguard_local_ip;
    String wireguard_remote_cidr;
    String wireguard_endpoint;
    uint16_t wireguard_endpoint_port;
    String wireguard_private_key;
    String wireguard_public_key;
    String wireguard_preshared_key;

    // Native Tailscale settings. Interactive browser enrollment does not use
    // an auth key. The legacy field remains write-only for migration only.
    bool tailscale_enabled;
    String tailscale_device_name;
    String tailscale_auth_key;

    // MQTT Gateway settings
    bool mqtt_enabled;          // Global enable/disable feature flag.
    bool mqtt_tls_insecure;     // True and ssl certificate errors are ignored.
    String mqtt_custom_ca;      // Optionally provide a custom CA public key.
};

// Initialize the Preferences (NVS)
void config_ui_init();

// Load the current configuration from NVS
BridgeConfig config_ui_load();

// Returns the write-only OTA API token stored redundantly in NVS. The token is
// never included in the HTTP configuration response or runtime logs.
String config_ui_get_ota_token();

// Start the Async Web Server for the captive portal
void config_ui_start_server();

// Stop the Async Web Server
void config_ui_stop_server();

// Temporarily closes the HTTP control/PhoneAPI listener during an official OTA
// download. Registered routes remain intact and can be reopened on failure.
void config_ui_pause_for_ota();
void config_ui_resume_after_ota();
