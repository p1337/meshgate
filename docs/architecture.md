# Meshtastic ESP32 Bluetooth-to-TCP Bridge

## Overview
This project is an ESP32-based transparent bridge that connects to a Meshtastic radio via Bluetooth Low Energy (BLE) and exposes it as a standard Meshtastic TCP connection over WiFi. This allows standard Meshtastic software (web UI, Python CLI, Android/iOS apps) to connect to the ESP32 over the local network as if it were a native network-connected Meshtastic device.

## Core Requirements
*   **Hardware Compatibility:** Maintain separate PlatformIO build profiles for the Seeed XIAO ESP32-S3, Generic ESP32 (WROOM-32), and Generic ESP32-S3 DevKit to cover 95% of active microcontrollers without code refactoring. ESP32-S2 is explicitly excluded due to lack of Bluetooth silicon.
*   **Bluetooth Connection:** Act as a BLE Central device, connecting to the Meshtastic radio (BLE Peripheral). Must support secure pairing with a PIN code.
*   **WiFi Connection:** Connect to a local WiFi network.
*   **TCP Server:** Expose a TCP port (default Meshtastic port is 4403) that accepts incoming connections.
*   **HTTP PhoneAPI:** Expose the standard naked-protobuf `/api/v1/toradio` and `/api/v1/fromradio` endpoints for browser clients.
*   **Transparent Bridging:** Seamlessly forward data back and forth between the TCP socket and the BLE connection.
*   **Offline Mailbox:** Retain decoded text messages while no app is connected and replay them through the native PhoneAPI after the next valid client configuration handshake.
*   **Configuration:** Provide a user-friendly mechanism to configure WiFi credentials, the target Meshtastic radio, and the BLE PIN code.

## Architecture & Design

### 1. Development & Build Environment
*   **Build System:** **PlatformIO** (PIO). It is widely used in the embedded space, handles dependencies automatically, and supports multiple ESP32 targets easily.
*   **Framework:** **Arduino Core for ESP32**. While ESP-IDF offers more control, the Arduino core provides mature, easy-to-use libraries for WiFi, BLE, and configuration portals, speeding up development while still being robust enough for a bridge.
*   **Environment:** **Docker / DevContainers**. We will set up a `.devcontainer` and a `Dockerfile` that includes the PlatformIO Core CLI. This ensures that any AI agent or developer can spin up the exact same environment without polluting their host machine.

#### ESP-IDF 5 and Tailscale migration

The `arduino_nano_esp32` profile remains the Arduino Core 2.0.17 / ESP-IDF
4.4.7 recovery build. The deployed `arduino_nano_esp32_idf5` profile targets
Arduino Core 3.3.9 / ESP-IDF 5.5.4 and uses NimBLE-Arduino 2.5.2. NimBLE 1.x
must not be used in that profile: it duplicates or incompatibly configures the
IDF-5 BLE stack and caused instruction/cache faults as soon as BLE started.
The 2.x callback, millisecond timeout, scan-result, and passkey APIs are selected
at compile time while the recovery profile retains its 1.x API.

MicroLink 2.0.0 at upstream commit
`216da3300f0493b0860247d43f7af5ce29df63a5` supplies native Tailscale support
and an enhanced lwIP WireGuard implementation. The IDF-5 profile deliberately
excludes `WireGuard-ESP32`; both classic WireGuard and Tailscale use MicroLink's
single WireGuard implementation. The two tunnel modes are mutually exclusive
at configuration time. MicroLink's Noise handshake uses the bundled reference
ChaCha20-Poly1305 adapter because the precompiled Arduino mbedTLS package does
not expose the required ChaChaPoly symbols.

Tailscale configuration is stored in both `bridge_cfg` and `bridge_backup`.
Legacy write-only auth keys remain preserved during migration but interactive
enrollment deliberately ignores them. A new node registers without credentials,
receives `AuthURL` from the control plane, exposes it through
`/tailscale_status`, and repeats registration with that URL in `Followup` until
the user approves it. The URL is RAM-only and is cleared after approval;
MicroLink's persistent machine, node/WireGuard and DISCO keys make subsequent
boots keyless. `connected` reports
control-plane registration, while `derp_connected` proves that the DERP TLS and
protocol handshakes completed and the relay data channel is usable. The same
response lists the known peer VPN IPs and whether each peer uses a direct or
relayed path. Joining a tailnet still requires explicit browser approval by an
authenticated user; the firmware cannot derive authorization from an email.

The control UI turns that protocol into one user action without weakening the
approval boundary. It synchronously pre-opens a same-browser waiting tab from
the **Add This Device to Tailscale** click, saves and reboots the gateway, then
polls `/tailscale_status` until `AuthURL` exists and navigates the waiting tab
to it. This avoids popup blocking after an asynchronous reboot and preserves a
manual link fallback for captive browsers. The original device page remains
open, polls authorization and exposes the resulting `100.x:4403` endpoint.

The Nano's local variant disables Arduino's stock double-tap recovery path.
That path expects a factory partition, while this project's 16 MB layout uses
two 6.25 MiB OTA application slots and has no factory application. Leaving the
stock recovery hook enabled caused a reboot loop after rapid resets.

The ESP32-S3 ROM/WebSerial download path may leave the Nano's active-low red
and blue RGB channels enabled after a successful installation. Production
startup explicitly configures all three RGB pins and drives them high before
the initial delay. Without this initialization the board can appear to remain
purple in bootloader mode even while MeshGate is already running its setup
access point.

Setup-mode diagnostics must distinguish an attempted access-point start from
a working radio. `wifi_net_start_ap()` checks the `WiFi.softAP()` result and
logs the assigned `192.168.4.1` address and AP MAC only after the driver has
confirmed success. A lone `Starting AP Mode` line is not proof that clients can
join the setup network.

The precompiled Arduino ESP-IDF 5 mbedTLS library normally allocates its TLS
record buffers from internal RAM. Running both Tailscale DERP and MQTTS then
fails with `mbedtls_ssl_setup() == -0x7F00`, even though the Nano has 8 MiB
PSRAM. MeshGate installs a PSRAM-aware mbedTLS allocator before either client
starts: allocations of at least 4 KiB prefer PSRAM, while small allocations
prefer internal RAM and both paths have a safe alternate heap. The MicroLink
DERP and coordination task stacks also use PSRAM. All application, network-I/O
and WireGuard-manager task stacks remain internal: task stacks can be inspected
while the flash cache is disabled, and putting them in PSRAM triggers
`esp_task_stack_is_sane_cache_disabled()` or an IPC stack-canary panic.

The IDF-5 profile reduces AsyncTCP's supported task stack from its 16 KiB
default to 4 KiB and uses a 6 KiB MQTT task stack. This preserves contiguous
internal RAM without weakening TLS or moving cache-sensitive task stacks. On
the production Nano, simultaneous DERP and verified MQTTS left about 50 KiB
heap free with a 24 KiB observed minimum during connection setup.

The Tailscale profile compiles the BLE/TCP queues as 4/4 packets, the MQTT
downlink queue as 2 packets, and the MicroLink peer table for four peers. This
leaves runtime headroom for DERP/WireGuard plus an active Meshtastic TCP client
while retaining complete-packet buffering in both directions. The deployed
Nano accepted three simultaneous port-4403 clients without queue drops. Legacy
profiles keep their original queue and peer sizing.

The MQTT gateway is enabled by default with MeshRadar's public shared uplink
profile (`meshradar.pl:8883`, TLS, root `msh`). Those published service values
are the factory standard. A complete custom `ModuleConfig.mqtt` received from the radio
replaces the broker, port, credentials, TLS mode and root topic. The radio must
still enable MQTT proxy-to-client and channel uplink for packets to reach the
bridge; a default broker connection cannot manufacture radio proxy envelopes.
BLE is not required for broker startup. If Tailscale is enabled, MQTT enters
`Waiting for Remote Access` until the DERP data channel is ready; MQTT/TLS then
starts without endangering the primary remote-administration path. Without
Tailscale, the standard broker session starts immediately after Wi-Fi.

ESP-IDF 5 also enforces lwIP core locking. MicroLink's WireGuard manager
registers its raw `netif` and UDP PCB while holding `LOCK_TCPIP_CORE`; direct
`netif_set_up()` or raw-PCB registration from the manager task otherwise trips
`LWIP_ASSERT_CORE_LOCKED` and causes a reboot loop immediately after startup.
Decrypted WireGuard payloads are likewise never passed directly to `ip_input()`
from the manager task. They go through the netif's `tcpip_input` callback, which
transfers ownership to lwIP's TCPIP thread. Bypassing that callback crashes on
the first inbound Tailscale data packet with the same core-lock assertion.

MapResponse UDP endpoints are path candidates, not proof of reachability. A
new WireGuard peer therefore starts with an empty endpoint and uses DERP. Only
an authenticated direct DISCO PONG installs a direct endpoint. Otherwise a
DERP-initiated mobile peer behind carrier NAT receives the handshake response
through DERP but subsequent IP replies are incorrectly sent to an unreachable
UDP candidate, producing a repeated handshake loop and no TCP connectivity.

#### Official release OTA channel

The normal Web UI never accepts a firmware file, URL or key. Its update action
starts an internal task which fetches `latest_ota.json` from the project's
GitHub Pages release channel over the embedded Mozilla CA bundle. The Pages
workflow creates that metadata from the latest GitHub Release and includes the
fixed production board, fixed firmware URL, byte length and SHA-256 digest.
Opening the normal connected control page starts the same metadata validation
in check-only mode. It never downloads or installs firmware automatically; it
only records whether the fixed official channel is current or newer. Setup AP
mode skips the network check so the captive portal remains fully offline.
Installed and available version values link to the repository's newest-first
technical `CHANGELOG.md`.
Firmware is streamed into the inactive OTA slot and hashed simultaneously;
the slot is activated only after all metadata, size and digest checks succeed.
The older authenticated raw endpoint is retained solely for development and
recovery and shares a process-wide lock with the official updater.

`CHANGELOG.md` is the release-note source of truth. The release workflow fails
when a firmware tag has no matching heading and publishes that technical entry
as the GitHub Release body. Documentation-only and promotional edits are not
firmware changelog entries.

The production updater pauses and destroys the active MQTT client before
opening its own TLS stream. This is required on the Nano ESP32 because a live
DERP session, MQTTS and OTA TLS otherwise exhausted internal heap during a
real 1.8 MB download (observed minimum: 300 bytes) and repeatedly closed the
stream near 400 KiB. Stored MQTT configuration is never changed and is restored
if the OTA attempt fails. The downloader also resumes an interrupted HTTPS
transfer with validated byte ranges, while one SHA-256 context spans every
connection; only the complete verified image can be activated.

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
    *   **HTTP:** Forward the same raw protobuf without TCP framing. A bounded
        polling queue retains `FromRadio` packets and applies BLE backpressure
        while the browser has not drained it yet.
    *   **Keep-alives:** Handle reconnections if the TCP client drops or the BLE radio goes out of range.

### 5. Persistent message mailbox

`message_mailbox.cpp` mounts LittleFS on the already provisioned `spiffs` data
partition; it does not change the partition table and does not consume the
small NVS partition used for credentials. The on-flash file is a fixed,
checksummed ring containing the latest 128 decoded `TEXT_MESSAGE_APP` packets.
Each record keeps the readable text and a replay-safe `FromRadio` protobuf.
Duplicate LoRa delivery is rejected by a RAM fingerprint index. When the ring
is full, the oldest record is replaced.

The BLE task treats an initialized mailbox as a permanent consumer, so the
radio is drained even with zero TCP/HTTP/MQTT clients. MQTT proxy envelopes,
configuration records, telemetry, node information and encrypted packets that
the paired radio cannot decode are not mailbox messages.

TCP clients are armed for replay only after they send `ToRadio.want_config_id`
and receive the matching `FromRadio.config_complete_id`. HTTP PhoneAPI uses the
same rule and serves retained records only after its live response queue has
delivered that completion marker. Replay snapshots a half-open sequence range,
so newly arriving packets do not turn one connection into an unbounded history
stream. The stored wrapper uses `FromRadio.id = 0` because that identifier is
local to the old radio FIFO session; the inner `MeshPacket.id` is preserved for
normal Meshtastic client deduplication and its `delayed` flag identifies the
replay as a delayed broadcast or direct message.

`GET /api/mailbox` exposes the same ring as JSON. `after` is an exclusive
sequence cursor and each response is capped at 32 messages to protect TLS/VPN
heap headroom. The endpoint reports ring bounds,
overwrite, duplicate and invalid-record counters. Ordinary reboot and OTA keep
the data partition; the explicit factory-reset endpoint clears the mailbox.

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
│   ├── message_mailbox.h  # Persistent message ring and replay interface
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
│   ├── message_mailbox.cpp # LittleFS mailbox, deduplication and replay records
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
