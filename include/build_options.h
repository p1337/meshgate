#pragma once

// ==============================================================================
// Build Options & Hardware Configuration
//
// All constants defined here can be safely overridden at compile-time by defining
// them in your platformio.ini `build_flags` (e.g. -D BOOT_BUTTON_PIN=15)
// ==============================================================================

// -----------------------------------------
// Hardware Pins
// -----------------------------------------
// The physical button used to force Setup Mode (Captive Portal).
// On 99% of ESP32 boards, this is the physical "BOOT" button on GPIO 0.
#ifndef BOOT_BUTTON_PIN
#define BOOT_BUTTON_PIN 0
#endif

// The built-in status LED used for visual blinking.
// If your Arduino board variant automatically defines STATUS_LED_PIN, it will use that.
// Otherwise, it falls back to this pin (21 is standard for Seeed XIAO ESP32S3).
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 21
#endif

// Does the LED turn ON when the pin is pulled LOW?
// Set to true for the Seeed XIAO ESP32S3, false for most standard dev boards.
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW true
#endif

// -----------------------------------------
// Network Settings
// -----------------------------------------
// The TCP port the bridge will listen on for incoming connections from Meshtastic Apps.
#ifndef TCP_PORT
#define TCP_PORT 4403
#endif

// How long before idle TCP connections will be disconnected.
#ifndef TCP_IDLE_TIMEOUT_SECONDS
#define TCP_IDLE_TIMEOUT_SECONDS 600
#endif

// Maximum number of simultaneous TCP clients (Apps) that can connect to the bridge simultaneously.
#ifndef MAX_TCP_CLIENTS
#define MAX_TCP_CLIENTS 3
#endif

// -----------------------------------------
// Bluetooth Settings
// -----------------------------------------
// Bluetooth Connect Timeout in seconds
#ifndef BLUETOOTH_TIMEOUT_SECONDS
#define BLUETOOTH_TIMEOUT_SECONDS 8
#endif

// Bluetooth Reconnect Delay time, in milliseconds
#ifndef BLUETOOTH_RECONNECT_DELAY_MS
#define BLUETOOTH_RECONNECT_DELAY_MS 5000
#endif

// Bluetooth Scan Timeout in seconds
#ifndef BLUETOOTH_SCAN_TIME_SECONDS
#define BLUETOOTH_SCAN_TIME_SECONDS 8
#endif

// Bluetooth Max devices to return from discovery scan.
#ifndef BLUETOOTH_MAX_DEVICES_DISCOVERABLE
#define BLUETOOTH_MAX_DEVICES_DISCOVERABLE 60
#endif

// -----------------------------------------
// Wifi Settings
// -----------------------------------------
// Wifi Max networks to return from discovery scan.
#ifndef WIFI_MAX_NETWORKS_DISCOVERABLE
#define WIFI_MAX_NETWORKS_DISCOVERABLE 30
#endif

// -----------------------------------------
// Bridge Settings
// -----------------------------------------
// Maximum binary packet size for Meshtastic ToRadio envelopes (512B payload + topic/metadata headers).
#ifndef MESHTASTIC_MAX_PACKET_SIZE
#define MESHTASTIC_MAX_PACKET_SIZE 576
#endif

// Size of message queues between BLE <--> TCP Connections.
#ifndef BRIDGE_QUEUE_SIZE
#define BRIDGE_QUEUE_SIZE 32
#endif

// Number of raw FromRadio protobufs retained for the browser HTTP PhoneAPI.
// This queue is independent from the immediately drained TCP broadcast queue;
// it must absorb packets while a browser completes each HTTP polling round trip.
#ifndef HTTP_PHONEAPI_QUEUE_SIZE
#define HTTP_PHONEAPI_QUEUE_SIZE 8
#endif

// The browser transport polls every three seconds. After this idle period the
// HTTP session is considered gone and its stale FromRadio queue is discarded.
#ifndef HTTP_PHONEAPI_ACTIVE_TIMEOUT_MS
#define HTTP_PHONEAPI_ACTIVE_TIMEOUT_MS 15000
#endif

// -----------------------------------------
// MQTT Gateway Settings
// -----------------------------------------
// Capacity of static MQTT downlink packet buffer queue.
#ifndef MQTT_QUEUE_SIZE
#define MQTT_QUEUE_SIZE 16
#endif

// Duration (in seconds) to keep MQTT broker connected during transient Bluetooth drops.
#ifndef MQTT_BLE_GRACE_PERIOD_SECONDS
#define MQTT_BLE_GRACE_PERIOD_SECONDS 60
#endif

// Underlying MQTT Client Keepalive setting, in seconds.
#ifndef MQTT_CLIENT_KEEPALIVE_SECONDS
#define MQTT_CLIENT_KEEPALIVE_SECONDS 60
#endif

// On disconnect from a broker, after how long will it attempt to reconnect, in milliseconds.
#ifndef MQTT_RECONNECT_TIME_MS
#define MQTT_RECONNECT_TIME_MS 15000
#endif

// Internal MQTT client buffer size
#ifndef MQTT_CLIENT_BUFFER_SIZE
#define MQTT_CLIENT_BUFFER_SIZE 2048
#endif

// Maximum allowable length for custom CA root certificate in PEM format (NVS limit is 4000)
#ifndef MQTT_MAX_CUSTOM_CA_LENGTH
#define MQTT_MAX_CUSTOM_CA_LENGTH 3500
#endif

// -----------------------------------------
// Firmware & Project Info
// -----------------------------------------
#ifndef FIRMWARE_VERSION
// Keep main on the next real release version. The release workflow injects
// the matching Git tag during its clean build. MeshGate does not distribute
// versionless/dev OTA images to end users.
#define FIRMWARE_VERSION "v0.7.2"
#endif

#ifndef HARDWARE_BOARD_NAME
#define HARDWARE_BOARD_NAME "generic"
#endif

#ifndef PROJECT_GITHUB_URL
#define PROJECT_GITHUB_URL "https://github.com/p1337/meshgate"
#endif
