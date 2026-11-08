# Project Status & Next Steps

## Completed So Far
*   **Architecture & Design:** Established the core plan (PlatformIO, Arduino Core, ESP32-S3 target).
*   **Captive Portal (Config UI):** Implemented the asynchronous web server (`config_ui.cpp`).
*   **WiFi / Boot Logic:** Implemented `wifi_net.cpp` (fallback to AP, mDNS broadcast).
*   **BLE Scanning & Connection Verification (`ble_client.cpp`):** Active scanning, MAC deduplication, background testing.
*   **Implement Normal Operation Mode BLE Connection (`ble_client.cpp`):** Handles connects, reconnects, and FromRadio/FromNum subscriptions.
*   **Implement TCP Bridge (`bridge.cpp`):** TCP server on port 4403, bidirectional routing, dynamic fast-polling, identical packet deduplication, Nagle's algorithm batched transmissions, robust queue backpressure, Mutex protection for concurrent TCP disconnects, and stack-allocated TCP frames to prevent heap fragmentation.
*   **Code Review & Refactoring:** Eliminated all heap fragmentation by changing `BridgePacket` to use a statically sized `uint8_t data[512]` array (trading ~50KB SRAM for memory safety) and refactored `bridge_init` to cleanly accept target arguments from the WiFi boot task.
*   **Dynamic configuration UI:** Added runtime toggle for serial debug logs to the captive portal.
*   **Dynamic mDNS naming:** The bridge automatically sanitizes the saved Bluetooth name and broadcasts it dynamically (e.g. `DSC_AE25-bridge.local`), conforming strictly to RFC 1035 length and character limits.
*   **Visual Status LED:** Added an asynchronous FreeRTOS LED task (`status_led.cpp`) mapping system states to blink patterns (Fast Blink: Boot, Slow Blink: AP Mode, Medium Blink: Bridge Searching, Solid On: Bridge Connected). Documented in `docs/user_guide.md`.
*   **Optional Standalone MQTT Broker Gateway / Proxy:** Implemented autonomous MQTT client subsystem (`mqtt_net.cpp`, `mqtt_net.h`), Nanopb compilation pipeline (`generate_protos.py`), WebUI configuration card with live radio sync telemetry, Auto-Sync from radio `ModuleConfig.mqtt`, 3-Tier TLS security (embedded Mozilla Root CA bundle, custom CA upload, and insecure bypass), and bidirectional BLE-to-MQTT multiplexing. Detailed in [docs/feature-mqtt-proxy.md](file:///Users/spowis/Documents/code/meshtastic-esp32-bt-bridge/docs/feature-mqtt-proxy.md).
*   **Distribution & Release Automation:** Built GitHub Actions CI/CD pipelines (`pr_check.yml`, `release.yml`) that automatically inject semver versions, compile multiple board profiles, and attach the binaries to GitHub Releases.
*   **Web Flasher & GitHub Pages:** Built a zero-install Web Flasher (`web/index.html`) using ESP Web Tools, and an automated GitHub Actions pipeline (`pages.yml`) to deploy it dynamically to GitHub Pages upon every new release.
*   **Documentation:** Authored comprehensive `README.md` and detailed architectural markdown files.
*   **Static FreeRTOS Queue Migration:** Migrated all inter-task FreeRTOS queues (`tcp_to_ble_queue`, `ble_to_tcp_queue`, `mqtt_to_ble_queue`) to `xQueueCreateStatic` with pre-allocated `.bss` buffers for 100% compile-time deterministic memory allocation and zero runtime heap fragmentation.
*   **Live Diagnostic & Queue Telemetry in WebUI:** Implemented consolidated wait-free `GET /status` endpoint and exposed live system metrics (uptime, heap memory, FreeRTOS task stacks, TCP clients, color-coded queue buffer capacity progress bars with drop counters, active IP/mDNS/RSSI, and MQTT telemetry) across UI cards with client-side polling controls. Detailed in [docs/feature-webui-metrics.md](file:///Users/spowis/Documents/code/meshtastic-esp32-bt-bridge/docs/feature-webui-metrics.md).
*   **Prometheus & OpenMetrics Exporter (`GET /metrics`) & Unified Diagnostics Engine:** Implemented standards-compliant Prometheus 0.0.4 exporter (`GET /metrics`) streaming line-by-line metrics (`meshbridge_*`) directly into lwIP network buffers via `AsyncResponseStream` with zero heap fragmentation. Unified all telemetry reporting (`GET /metrics`, `GET /status`, and periodic 15s Serial logger) into `diag_telemetry` with symmetric traffic counters (RX, TX, Invalid) across BLE, TCP, and MQTT, and isolated capacity queue drops. Detailed in [docs/feature-prometheus-metrics.md](file:///Users/spowis/Documents/code/meshtastic-esp32-bt-bridge/docs/feature-prometheus-metrics.md) and [docs/user_guide.md](file:///Users/spowis/Documents/code/meshtastic-esp32-bt-bridge/docs/user_guide.md).

## Hardware Profiles Supported
*   **Seeed XIAO ESP32-S3** (`seeed_xiao_esp32s3`): Native USB, Active Low LED.
*   **Generic ESP32** (`esp32dev`): Standard WROOM-32, NodeMCU. Hardware UART, Active High LED.
*   **Generic ESP32-S3 DevKit** (`esp32-s3-devkitc-1`): Standard S3 devkit. Native USB.

## Outstanding Features & Future Roadmap
*   **Completed — authenticated OTA updates:** The Arduino Nano ESP32 profile uses dual OTA slots and a protected raw HTTP upload API with MD5 verification and an automated PowerShell uploader.
*   DRY out notifyFromRadio() and bridgeBleTask() in bridge.cpp?
*   **MQTT Proxy Fallback Policy:** Decide whether `MqttClientProxyMessage` uplink packets should be dropped or allowed to fall through to TCP clients when the bridge's MQTT Gateway is enabled but the broker connection is temporarily offline.
