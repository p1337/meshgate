# Configuration UI Design (Captive Portal)

## Overview
To provide a seamless setup experience without requiring hard-coded credentials, the ESP32 bridge implements a Captive Portal. This allows the user to connect to the ESP32's own WiFi network and configure both the local network (WiFi) and the target Bluetooth (BLE) settings from a mobile device or laptop browser.

The UI is built with a dark, modular card-based interface that follows the
MeshRadar visual language: near-black panels, green operational accents,
orange highlights, compact uppercase labels, and monospaced status values. The
embedded version uses only local system-font fallbacks so the captive portal
remains fully functional without internet access. It allows inspecting current
settings, updating WiFi and Bluetooth parameters independently, scanning for
nearby BLE devices, and verifying live BLE connectivity and PIN pairing before
rebooting into normal bridge mode.

## User Interface & Features

### 1. Modular Card Design
* **WiFi Network Card:**
  * Displays the current configured SSID, masked password status, live WiFi connection status pill (`Connected` / `AP Mode` / `Disconnected` / `Not Configured`), active IP address, advertised mDNS hostname (e.g. `dsc-ae28.local`), and live WiFi RSSI signal strength with colored quality pill (`Excellent` / `Good` / `Fair` / `Weak`).
  * Displays active **TCP Clients** (`X / 3`) and live **TCP Traffic** counters (sent, received, invalid).
  * In Edit mode, offers a live scan dropdown of nearby WiFi networks (with RSSI signal levels and lock/open security indicators), manual SSID entry for hidden networks, and Password input with a **Show/Hide** toggle.
  * Features a **🔍 Scan for Networks** button (with scan in progress indicator and scan/save buttons disabled during scans).
  * Independent **Save WiFi** button persists network credentials without affecting BLE settings.
* **Bluetooth Target Card:**
  * Displays the configured target device: `Target Device: <Name> (<MAC>)` (or `(<MAC>)` if unnamed), write-only PIN presence, live Bluetooth connection status pill (`Connected` / `Disconnected`), and live Bluetooth **Traffic** counters (sent, received, invalid).
  * In Edit mode, offers a live scan dropdown with RSSI signal strengths, manual MAC entry, and 6-digit PIN input.
  * Features a **⚡ Test Connection** button to verify BLE pairing and Meshtastic GATT services live.
  * Independent **Save Bluetooth** button persists target device name, MAC, and PIN.
* **MQTT Gateway Card:**
  * Displays feature enable/disable toggle, live radio sync status, active broker address/port/TLS mode, Client Status pill, and live MQTT **Traffic** counters (sent, received, invalid).
  * In Edit mode, offers:
    * **Enable MQTT Gateway** toggle.
    * **Skip Certificate Validation** toggle (for self-signed / local LAN brokers).
    * **Custom CA Root Certificate** textarea for uploading private PEM certificates.
  * Independent **Save MQTT Settings** button persists settings to NVS and applies them immediately.
* **Tailscale Card (IDF-5 Nano build):**
  * Displays enablement, live connection state, device name, assigned VPN IPv4 address, known peer count, and enrollment state.
  * Includes a self-contained three-step one-click setup wizard with an
    offline Tailscale download QR code and direct iOS, Android, Windows and
    macOS install buttons. No auth/API key is copied.
  * **Add This Device to Tailscale** pre-opens a browser tab from the user's
    click, saves the configuration, reboots MeshGate, polls the local device
    during recovery and redirects that tab to the one-time control-plane URL.
    The original page remains open and reports approval automatically. A
    visible login link remains as the fallback for captive browsers that block
    popups.
  * After enrollment, displays copyable Web UI and Meshtastic PhoneAPI
    addresses (`http://100.x/` and `100.x:4403`) with a short Meshtastic
    connection instruction. Machine/key-expiry management is optional.
  * Accepts a lowercase device name. Existing legacy auth-key values remain
    write-only and preserved but are ignored by interactive enrollment.
  * Tailscale and classic WireGuard are mutually exclusive because both share the same embedded WireGuard engine.
* **System Actions Card:**
  * Displays formatted **System Uptime** (e.g. `1h 24m 12s`).
  * Displays **Memory (Heap)** stats: Free Heap, Minimum Recorded Free Heap, and Maximum Allocatable Block in KB.
  * Displays **Free Task Stacks** high-water mark headroom for `bridge_ble` and `bridge_net` tasks.
  * Displays **Queue Buffers**: Visual color-coded capacity progress bars (`< 50%` Green, `50-80%` Yellow, `> 80%` Red), message counts (`waiting / capacity`), and drop counters for `TCP ➔ BLE`, `BLE ➔ TCP`, and `MQTT ➔ BLE` queues.
  * Contains a toggle for **Live Telemetry Polling (7s)** (enabled by default, with dynamic 7s ➔ 3s ➔ 1s rate cycling via the <kbd>P</kbd> key).
  * Contains a toggle for **Enable Serial Debug Logs**.
  * Contains a **Reboot & Start Bridge** action that restarts the ESP32 into normal runtime mode.
  * Contains a **Reset All Settings** action (with confirmation dialog) that clears all stored NVS credentials and restarts into Setup Mode.
* **Firmware OTA Update Card:**
  * Shows the running firmware version and active/next application slots.
  * Provides one **Check & Install Latest Release** action with visible state
    and download progress; no file chooser or user key is shown.
  * Downloads only the production ESP32-S3 image named by the official release
    metadata and verifies TLS, board identity, size and SHA-256 before activation.
* **Footer:**
  * Dynamically embeds the compiled Firmware Version and links to the project's GitHub repository.

### 2. Live WiFi & BLE Scanning
* **WiFi Scanning:** Runs asynchronously via `WiFi.scanNetworks(true)` in `WIFI_AP_STA` mode without dropping the captive portal AP connection. Discovered networks are deduplicated and ordered by signal strength (RSSI).
* **BLE Scanning:** Initiated via **🔍 Scan for Devices** (or automatically when expanding BLE edit mode). Runs an active 4-second BLE scan with duplicate filtering, sorted with named devices first.
* Both scanners show `-- Scan in progress... --` and disable action buttons during active scans to prevent radio contention.

### 3. Live Connection & PIN Verification
* Clicking **⚡ Test Connection** connects to the specified MAC address and authenticates with the provided 6-digit PIN.
* **Passkey Security & Bond Clearing:** Clears any existing bond (`deleteBond`) before and after the test to ensure that the PIN is genuinely challenged and validated on every test run.
* **Service & Characteristic Check:** Confirms the presence of the Meshtastic Service UUID (`6ba1b218-15a8-461f-9fa8-5dcae273eafd` or `cb0b9a0b-a8c2-49c0-bdd5-3fa12b04d84b`) and required `ToRadio` / `FromRadio` characteristics.
* **Non-Blocking Execution:** Runs in a dedicated FreeRTOS background task on Core 1 to ensure the asynchronous web server on Core 0 remains completely responsive.

## System Workflow & Endpoints

1. **AP Mode Initialization (`wifi_net.cpp`)**:
   * The ESP32 starts an open WiFi AP (`Meshtastic-Bridge-Setup` at `192.168.4.1`) in `WIFI_AP_STA` mode whenever:
     * The physical `BOOT` button is pressed during the 3-second startup window (or held for 2 seconds at runtime).
     * No WiFi credentials (`wifi_ssid`) are configured.
     * No Bluetooth target device (`ble_mac`) is configured.
     * Connecting to the configured WiFi network fails during normal boot.
   * A `DNSServer` intercepts DNS queries and redirects captive portal clients to `http://192.168.4.1/`.

2. **Web Server Endpoints (`config_ui.cpp`)**:
   * **`GET /`**: Serves the single-page HTML/CSS/JS application directly from flash (`PROGMEM`) via `AsyncProgmemResponse` with zero heap allocation.
   * **`GET /config`**: Returns current settings as JSON, including WiFi/BLE/MQTT/WireGuard values plus `tailscale_enabled`, `tailscale_supported`, `tailscale_device_name`, and `tailscale_has_auth_key`. Secret WiFi, BLE PIN, WireGuard, OTA, and Tailscale values are never returned. `ble_has_pin` reports only whether a PIN is stored.
   * **`GET /status`**: Returns unified live diagnostic and telemetry JSON polled every 7 seconds by the client: contains `uptime_seconds`, `heap` (free, min_free, max_alloc), `wifi` (connected, is_ap_mode, ip, rssi, mdns_host), `ble` (connected, rx_count, tx_count, invalid_count), `tcp` (connected_clients, max_clients, rx_count, tx_count, invalid_count), `traffic` (ble, tcp, mqtt rx/tx/invalid), `queues` (waiting, capacity, dropped for `tcp_to_ble`, `ble_to_tcp`, `mqtt_to_ble`), `stacks` (ble_task, net_task free bytes), and `mqtt` (gateway_enabled, state, radio_proxy_enabled, broker, root, published, received, dropped, invalid).
   * **`GET /metrics`**: Standards-compliant Prometheus / OpenMetrics scraping endpoint (`text/plain; version=0.0.4; charset=utf-8`). Streams line-by-line metrics (`meshbridge_*`) directly into lwIP network output buffers via `AsyncResponseStream` with zero large heap allocations. Suitable for scraping by Prometheus, VictoriaMetrics, Grafana Agent, and Home Assistant.
   * **`OPTIONS|PUT /api/v1/toradio`**: Standard Meshtastic HTTP PhoneAPI write endpoint. The PUT body is one bounded raw `ToRadio` protobuf and returns `204` after it is queued for BLE.
   * **`OPTIONS|GET /api/v1/fromradio`**: Standard Meshtastic HTTP PhoneAPI polling endpoint. Returns one raw `FromRadio` protobuf, an empty body, or all queued packets when `all=true`. Responses include CORS and Private Network Access headers for local browser clients.
   * **`GET /start_scan_wifi`**: Initiates an asynchronous WiFi network scan.
   * **`GET /scan_wifi_results`**: Polls WiFi scan progress and returns JSON array of discovered networks: `[{"ssid":"MyWiFi","rssi":-58,"is_open":false}]`.
   * **`GET /start_scan`**: Initiates an asynchronous 4-second BLE scan.
   * **`GET /scan_results`**: Polls scan progress and returns JSON array of discovered devices: `[{"name":"Meshtastic_xxxx","mac":"AA:BB:CC:DD:EE:FF","rssi":-68}]`.
   * **`POST /start_test_ble`**: Launches a background FreeRTOS task to test BLE pairing and GATT service discovery. An empty PIN reuses the stored value without returning it to the browser.
   * **`GET /test_ble_status`**: Polls the test task state and returns result JSON (`{"status":"done","success":true,"message":"..."}`).
   * **`POST /save_wifi`**: Saves `wifi_ssid` and `wifi_pass` to NVS.
   * **`POST /save_ble`**: Saves `ble_name`, `ble_mac`, and a validated six-digit `ble_pin` to NVS. An empty PIN preserves the stored value.
   * **`POST /save_mqtt`**: Saves `mqtt_enabled`, `mqtt_tls_insec`, and `mqtt_custom_ca` to NVS and applies changes live if in Normal mode.
   * **`POST /save_tailscale`**: Saves `enabled` and `device_name` redundantly. A reboot starts browser enrollment or reconnects the persistent identity.
   * **`GET /tailscale_status`**: Reports `enabled`, `connected`, state name, assigned VPN IP, peer count and a pending RAM-only `auth_url` while browser approval is required.
   * **`POST /save_system`**: Saves `debug_logs` boolean flag to NVS.
   * **`POST /reboot`**: Restarts the ESP32 into normal bridge mode.
   * **`POST /reset`**: Erases all stored NVS configurations and reboots the ESP32 into Setup Mode.
   * **`POST /save`**: Full-form save endpoint for backward compatibility.
   * **`GET /api/ota`**: Reports OTA readiness, firmware/board identity, active and next application partitions, and maximum image size. It never returns the OTA token.
   * **`POST /api/ota`**: Accepts a raw firmware binary authenticated by `X-OTA-Key`; the deployment uploader also supplies `X-Firmware-MD5`. A validated image is activated and the bridge reboots.
   * **`POST /api/ota/token`**: Stores a new write-only OTA key in both NVS
     namespaces and applies it immediately. It is accepted only in setup mode
     from the setup AP's own subnet, never from LAN or VPN clients.
   * **`GET /api/ota/official`**: Reports official update support, current and
     latest version, lifecycle state, byte progress and a bounded error string.
   * **`POST /api/ota/official`**: Starts the background official-release
     download and install task. It accepts no URL, file or credential from the
     browser, so it cannot be used to install arbitrary firmware.

The `/api/ota/token` and `/api/ota/official` handlers must be registered before `/api/ota`.
ESPAsyncWebServer considers a route to match slash-delimited child paths, so
registering the parent first would shadow the provisioning endpoint with the
raw firmware-body handler.

## Memory & Non-Volatile Storage (NVS)
The following keys are stored in the `Preferences` namespace (`bridge_cfg`):
* `wifi_ssid` (String)
* `wifi_pass` (String)
* `ble_name` (String)
* `ble_mac` (String)
* `ble_pin` (String)
* `debug_logs` (Bool)
* `mqtt_enabled` (Bool)
* `mqtt_tls_insec` (Bool)
* `mqtt_custom_ca` (String)
* `ota_token` (64-character hexadecimal String, write-only and duplicated in `bridge_backup`)
* `ts_enabled` (Bool, duplicated in `bridge_backup`)
* `ts_name` (String, duplicated in `bridge_backup`)
* `ts_auth` (String, write-only Tailscale auth key, duplicated in `bridge_backup`)

## Technical Decisions & Considerations
* **Captive Portal / WebView Compatibility Constraints (macOS / iOS CNA & Windows):**
  * The WebUI is frequently opened inside constrained OS Captive Portal WebViews (e.g. Apple's Captive Network Assistant `CaptiveNetworkSupport`, Windows Captive Portal, Android CaptivePortalLogin).
  * **No Native `window.alert()` / `window.confirm()` / `window.prompt()`:** Many OS captive browser engines completely suppress, ignore, or hang on native browser dialogs. All notifications, alerts, and confirmation dialogs must use **custom in-DOM HTML/CSS modals** (e.g., `#modal-overlay`, `.alert-box`) rather than native JavaScript dialogs.
  * **Self-Contained Offline Assets:** In AP Setup mode, the client device has no upstream internet access. The UI must never reference external CDN scripts, CSS files, or web fonts; all CSS, SVGs, HTML, and JavaScript must be entirely self-contained within `PROGMEM` flash memory.
  * **No External Navigation / New Tabs:** Popups or links targeting `_blank` may be blocked or forcibly close the CNA window.
  The Tailscale wizard therefore shows copyable destination URLs as a fallback;
  users can open them in a normal internet-connected browser when the captive
  setup popup cannot launch an external tab.
  * **Stateless Client Session:** OS captive popups often discard cookies and `localStorage` when closed; all state must reside in ESP32 NVS or be queried dynamically from `/config` and `/status`.
* **Zero-Heap PROGMEM Serving:** The main web UI HTML payload is streamed directly out of flash memory via `AsyncProgmemResponse`, eliminating the ~35 KB temporary heap spike when serving web clients.
* **Client-Side Polling:** The unified `/status` endpoint is polled periodically (defaulting to 7s, dynamically adjustable to 3s or 1s via the <kbd>P</kbd> key) using chained asynchronous requests to maintain real-time telemetry while completely avoiding request stacking and minimizing CPU/network overhead.
* **Radio Concurrency:** The ESP32 shares a single 2.4GHz radio antenna for WiFi and Bluetooth. Using `ESPAsyncWebServer` combined with asynchronous background tasks for BLE operations prevents HTTP request timeouts and prevents radio collisions while switching between WiFi AP and BLE scanning/testing.
* **NimBLE Stack:** `NimBLE-Arduino` provides low memory footprint BLE client capabilities, essential for coexisting with `ESPAsyncWebServer` and `WiFi` on resource-constrained ESP32-S3 boards without heap exhaustion.
