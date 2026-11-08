# Meshtastic ESP32 Bluetooth-to-TCP Bridge

## Overview
This project is an ESP32-based transparent bridge that connects to a Meshtastic radio via Bluetooth Low Energy (BLE) and exposes it as a standard Meshtastic TCP connection over WiFi. This allows standard Meshtastic software (web UI, Python CLI, Android/iOS apps) to connect to the ESP32 over the local network as if it were a native network-connected Meshtastic device.

## Core Requirements
*   **Hardware Compatibility:** Maintain separate PlatformIO build profiles for the Seeed XIAO ESP32-S3, Generic ESP32 (WROOM-32), and Generic ESP32-S3 DevKit to cover 95% of active microcontrollers without code refactoring. ESP32-S2 is explicitly excluded due to lack of Bluetooth silicon.
*   **Bluetooth Connection:** Act as a BLE Central device, connecting to the Meshtastic radio (BLE Peripheral). Must support secure pairing with a PIN code.
*   **WiFi Connection:** Connect to a local WiFi network.
*   **TCP Server:** Expose a TCP port (default Meshtastic port is 4403) that accepts incoming connections.
*   **Transparent Bridging:** Seamlessly forward data back and forth between the TCP socket and the BLE connection.
*   **Configuration:** Provide a user-friendly mechanism to configure WiFi credentials, the target Meshtastic radio, and the BLE PIN code.

## Architecture & Design

### 1. Development & Build Environment
*   **Build System:** **PlatformIO** (PIO). It is widely used in the embedded space, handles dependencies automatically, and supports multiple ESP32 targets easily.
*   **Framework:** **Arduino Core for ESP32**. While ESP-IDF offers more control, the Arduino core provides mature, easy-to-use libraries for WiFi, BLE, and configuration portals, speeding up development while still being robust enough for a bridge.
*   **Environment:** **Docker / DevContainers**. We will set up a `.devcontainer` and a `Dockerfile` that includes the PlatformIO Core CLI. This ensures that any AI agent or developer can spin up the exact same environment without polluting their host machine.

#### ESP-IDF 5 and Tailscale migration

The verified production profile is currently `arduino_nano_esp32` on Arduino
Core 2.0.17 / ESP-IDF 4.4.7. The `arduino_nano_esp32_idf5` profile is an
explicit migration environment targeting Arduino Core 3.3.9 / ESP-IDF 5.5.4.
It exists so IDF-5 API changes and library compatibility can be resolved without
destabilizing the deployable firmware.

MicroLink is the selected native Tailscale implementation for the ESP32-S3.
Its integration is not complete yet. MicroLink contains an enhanced lwIP
WireGuard implementation with public symbols that overlap the currently used
`WireGuard-ESP32` library. The migration must therefore use one shared
WireGuard implementation for classic WireGuard and Tailscale; linking both
libraries into the same firmware is not a valid architecture. Existing
WireGuard configuration and behavior must remain available throughout that
migration.

### 2. Key Libraries
*   **BLE:** `h2zero/NimBLE-Arduino`. NimBLE is a highly optimized, low-RAM Bluetooth stack. It is much more stable and resource-efficient than the standard Bluedroid stack, and it fully supports secure pairing and PIN codes.
*   **WiFi:** Standard `WiFi.h`.
*   **Configuration:** A custom AsyncWebServer (e.g., using `ESPAsyncWebServer`) to host the captive portal.
*   **Storage:** `Preferences` (NVS) to save the configuration persistently.

### 3. Configuration Flow (Captive Portal)
To provide the best user experience, configuration is handled via a web-based captive portal:
1.  **Setup Mode Trigger:** On first boot, or if a physical "Reset" button is held down for a few seconds, the ESP32 enters Setup Mode.
2.  **AP Mode:** The ESP32 broadcasts its own WiFi Access Point (e.g., `Meshtastic-Bridge-Setup`).
3.  **Web UI & BLE Scanning:** When a user connects to this AP and opens the portal:
    *   The ESP32 performs a BLE scan in the background for devices advertising the Meshtastic service UUID.
    *   The web page presents a form containing:
        *   **WiFi Credentials:** SSID and Password fields (to configure or re-configure local network access).
        *   **Target Radio:** A dropdown list of discovered Meshtastic BLE radios to select as the target.
        *   **BLE PIN:** A text input for the 6-digit BLE PIN code (displayed on the radio during pairing).
4.  **Save & Reboot:** Upon submission, the ESP32 saves the WiFi credentials, Target BLE MAC, and BLE PIN to non-volatile storage (NVS) and reboots into normal operation mode.

### 4. System Flow (Normal Operation)
1.  **Boot & Init:** Initialize NVS and read stored configuration.
2.  **Network Setup:**
    *   Attempt to connect to the configured WiFi.
    *   Once connected to WiFi, start the TCP Server (listening on port 4403).
3.  **Bluetooth Setup:**
    *   Initialize NimBLE in Central mode.
    *   Set the security callbacks to provide the configured PIN code when the Meshtastic radio requests it.
    *   Connect to the saved Target BLE MAC address.
    *   Subscribe to the Meshtastic `FromRadio` BLE characteristic (Notifications).
4.  **Bridging Loop:**
    *   **TCP -> BLE:** When bytes arrive from a connected TCP client, read them into a buffer and write them to the Meshtastic `ToRadio` BLE characteristic. (Note: must respect BLE MTU limits and chunk the data if necessary).
    *   **BLE -> TCP:** When a notification is received from the `FromRadio` characteristic, immediately write those bytes out to the connected TCP client socket.
    *   **Keep-alives:** Handle reconnections if the TCP client drops or the BLE radio goes out of range.

## Project Structure
```text
meshtastic-esp32-bt-bridge/
├── .devcontainer/         # Docker dev environment definitions
│   └── devcontainer.json
├── .github/workflows/     # CI/CD pipelines (pr_check.yml, pages.yml, release.yml)
├── data/
│   └── cert/              # Embedded Mozilla Root CA binary bundle (x509_crt_bundle.bin)
├── docs/                  # Additional documentation
│   ├── architecture.md    # System architecture
│   ├── bridge_architecture.md # Network & Bridge details
│   ├── configuration_ui.md# Captive portal design
│   ├── development.md     # Build guide, queue sizing & release instructions
│   ├── feature-mqtt-proxy.md # Standalone MQTT Gateway & Proxy architecture
│   ├── feature-prometheus-metrics.md # Prometheus / OpenMetrics scraper specification
│   ├── feature-webui-metrics.md # Live Diagnostic Metrics in WebUI
│   └── user_guide.md      # End-user manual and LED reference
├── include/               # Header files
│   ├── bridge.h
│   ├── build_options.h
│   ├── config_ui.h
│   ├── diag_telemetry.h   # Periodic health and queue diagnostics
│   ├── mqtt_net.h         # MQTT subsystem interface & state types
│   ├── ota_update.h       # Authenticated HTTP OTA endpoint
│   ├── status_led.h
│   ├── utils.h
│   └── wifi_net.h
├── src/                   # C++ Source code
│   ├── main.cpp           # Main application loop
│   ├── bridge.cpp         # Logic for bridging TCP, MQTT, and BLE streams (dual queues)
│   ├── ble_client.cpp     # NimBLE client and security callbacks
│   ├── diag_telemetry.cpp # Low-priority periodic telemetry logger
│   ├── mqtt_net.cpp       # MQTT Gateway subsystem (Auto-Sync, 3-tier TLS)
│   ├── ota_update.cpp     # Dual-slot OTA writer and image validation
│   ├── wifi_net.cpp       # WiFi, AP Mode, and mDNS management
│   ├── config_ui.cpp      # Captive portal / Preferences logic
│   └── status_led.cpp     # Asynchronous LED visual indicators
├── web/                   # Zero-install Web Flasher UI (index.html)
└── platformio.ini         # PlatformIO build configurations
```

## AI Agent Instructions
When working on this project:
1. Always refer to this `docs/architecture.md` file for context on the tech stack.
2. Ensure new dependencies are added to `platformio.ini`.
3. Use NimBLE for all Bluetooth operations.
4. Keep memory management in mind; Meshtastic packets can be large, so avoid excessive dynamic memory allocation (`String`, `malloc`) in the fast-path bridging loop.
