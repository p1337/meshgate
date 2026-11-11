#include <Arduino.h>
#include "config_ui.h"
#include "wifi_net.h"
#include "mqtt_net.h"
#include "status_led.h"
#include "bridge.h"
#include "build_options.h"
#include "utils.h"
#include "diag_telemetry.h"
#include "wireguard_net.h"
#include "tailscale_net.h"
#include "reliability_manager.h"
#include "web_log.h"
#include "ip_led.h"
#include "message_mailbox.h"
#include "device_presence.h"

/**
 * Main Entry point.
 */
void setup() {
    // Start serial console and enable sending log output to it.
    Serial.begin(115200);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    // An enumerated Windows USB CDC device can stop consuming data when no
    // terminal is open. Never let that stall unattended network startup.
    // The Arduino HWCDC implementation underflows its retry counter at exactly
    // zero, so use the smallest safe non-blocking-equivalent timeout.
    Serial.setTxTimeoutMs(1);
#endif
    Serial.setDebugOutput(true);
    web_log_init();

    // WebSerial/ROM download mode can leave the Nano RGB LED purple. Clear it
    // before any startup delay so a successful boot never resembles a stuck
    // bootloader or failed installation.
    ip_led_init();

    // Initialize Status LED,
    status_led_init();

    // Configure "boot button" pin and setup on/off as high/low depending on board.
#if BOOT_BUTTON_PIN >= 0
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
#endif

    // Initial delay waiting for serial to start, USB to connect and catch up.
    delay(2000);
    log_i("\n--- Starting Meshtastic ESP32 BT-TCP Bridge ---");

    // Initialize MQTT subsystem and locks
    mqtt_net_init();

    // Load stored configuration properties from NVS
    config_ui_init();
    message_mailbox_init();
    BridgeConfig cfg = config_ui_load();
    device_presence_init();

    // Set global ESP-IDF log level based on UI setting
    if (cfg.debug_logs) {
        log_i("Debug logging enabled!");
    }
    utils_set_debug_logging(cfg.debug_logs);

    // Determine which boot mode to enter.
    // Check if BOOT button is held during power-on.
    // If the boot button is pressed, or we are missing configuration properties, it will
    // enter "setup" mode and start up an Access Point with the configuration web UI.
    bool forceSetup = false;
    bool btnPressed = false;
#if BOOT_BUTTON_PIN >= 0
    btnPressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);
#endif

    // Start Fast Blink to indicate that we're booting up.
    status_led_set(LED_FAST_BLINK);

    // Wait up to 5 seconds for setup button to be pressed.
    if (BOOT_BUTTON_PIN >= 0 && !btnPressed && !cfg.wifi_ssid.isEmpty() && !cfg.ble_mac.isEmpty()) {
        log_i("[BOOT] Press button in next 5 seconds to enter setup mode...");

        unsigned long startWait = millis();
        while (millis() - startWait < 5000) {
#if BOOT_BUTTON_PIN >= 0
            if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
                log_i("[BOOT] Entering Setup Mode (button pressed)");
                btnPressed = true;
                break;
            }
#endif
            delay(50);
        }
    }

    // Log if we're going to enter setup mode, and why.
    if (btnPressed) {
        forceSetup = true;
    } else if (cfg.wifi_ssid.isEmpty()) {
        log_i("[BOOT] Entering Setup Mode (No saved WiFi credentials)");
        forceSetup = true;
    } else if (cfg.ble_mac.isEmpty()) {
        log_i("[BOOT] Entering Setup Mode (No Bluetooth target configured)");
        forceSetup = true;
    }

    if (forceSetup) {
        // Enter Setup mode.
        wifi_net_start_ap();
        // If WiFi was already saved, connect it immediately while keeping the
        // setup AP available for unfinished Bluetooth/WireGuard configuration.
        if (!cfg.wifi_ssid.isEmpty()) {
            wifi_net_connect_ap_sta_async(cfg.wifi_ssid, cfg.wifi_pass);
        }
    } else {
        // Normal boot, log config
        log_i("\n==========================================");
        log_i("[BOOT] Mode: Normal Operating Mode");
        log_i("[BOOT] Target WiFi: configured");
        log_i("[BOOT] Target BLE:  configured");
        log_i("==========================================");

        // Start Medium Blink to indicate 'searching for bluetooth and wifi'
        status_led_set(LED_MED_BLINK);

        // Connect to wifi
        if (wifi_net_connect_sta(cfg.wifi_ssid, cfg.wifi_pass)) {
            // Start mDNS advertising.
            wifi_net_start_mdns(cfg);

            // Start up the captive portal web server.
            config_ui_start_server();

            // Register this installation before the memory-heavy BLE, VPN and
            // MQTT services start. Periodic refreshes reuse the Arduino loop
            // instead of reserving another FreeRTOS task stack.
            device_presence_start();

            // Init and start bridge BLE <--> WIFI
            bridge_init(cfg.ble_name, cfg.ble_mac, cfg.ble_pin.toInt());
            bridge_start();

            // Start periodic diagnostic telemetry task (Core 0, Priority 1, every 15s)
            diag_telemetry_start();

            // Establish the outbound VPN tunnel without blocking local bridge
            // operation, then supervise all critical links and scheduled reboots.
            if (cfg.tailscale_enabled) {
                tailscale_net_start(cfg);
            } else {
                wireguard_net_start(cfg);
            }
            reliability_manager_start(cfg);

            // MQTT is deliberately applied after remote access startup. Its
            // TLS allocation is deferred until Tailscale DERP is established.
            MqttConfig mqttCfg;
            mqttCfg.enabled = cfg.mqtt_enabled;
            mqttCfg.tls_insecure = cfg.mqtt_tls_insecure;
            mqttCfg.custom_ca = cfg.mqtt_custom_ca;
            mqtt_net_apply_config(mqttCfg);
        } else {
            log_i("[BOOT] WiFi Connection Failed! Falling back to Setup Mode.");
            wifi_net_start_ap();
        }
    }
}

//  Main program loop.
void loop() {
    wifi_net_loop();
    mqtt_net_loop();
    device_presence_loop();
}
