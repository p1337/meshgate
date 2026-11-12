# Development & Build Guide

This project is built using **PlatformIO**. Because you are using IntelliJ on macOS, the easiest and most reliable way to build and flash the ESP32 is to use the PlatformIO Core Command Line Interface (CLI) directly in your IntelliJ terminal. 

*(Note: While we have a Docker DevContainer for pure compiling, flashing a USB device from inside a Docker container on macOS is notoriously difficult due to USB passthrough limitations. Therefore, a local installation of the CLI is highly recommended).*

## 1. Prerequisites (macOS)
You can install the PlatformIO CLI locally on your Mac using Homebrew:
```bash
brew install platformio
```
*(Alternatively, if you use Python, you can run `pip install platformio`)*

### Protobuf & Submodule Setup
This project uses the official Meshtastic Protobufs submodule and Nanopb. Ensure submodules are checked out and the required Python tools are installed:
```bash
# Initialize and fetch git submodules
git submodule update --init --recursive

# Install required Python protobuf generation packages
pip install protobuf grpcio-tools
```

PIOArduino creates a second Python environment at `~/.platformio/penv` while
installing the IDF-5 platform. Clean CI runners must install the packages into
that environment too, after `pio pkg install` has created it. Installing them
only into the outer runner Python makes `generate_protos.py` fail even though
`pip` reports them as installed.

Verify the installation by running:
```bash
pio --version
```

## 2. IntelliJ Setup (Code Completion)
To get IntelliJ (or CLion) to understand the C++ Arduino framework and resolve your `#include` headers properly, you can have PlatformIO generate the necessary IDE configuration files (like `CMakeLists.txt` for CLion).

In your IntelliJ terminal at the root of the project, run:
```bash
pio init --ide clion
```
*(If you are using standard IntelliJ IDEA, you can try `--ide clion`. It will generate the necessary `.iml` and workspace files).*

## 3. Building the Project
To compile the C++ code without flashing it (useful for checking for syntax errors):
```bash
pio run
```
PlatformIO will automatically download the required toolchains for the ESP32-S3 and the libraries defined in `platformio.ini` (like `NimBLE-Arduino` and `ESPAsyncWebServer`).

For the Arduino Nano ESP32 IDF-5/MicroLink deployment build, use the explicit
environment so PlatformIO selects Arduino Core 3.3.9, ESP-IDF 5.5.4,
NimBLE-Arduino 2.5.2, and the vendored MicroLink component:

```powershell
pio run -e arduino_nano_esp32_idf5
```

On Windows with the repository's PlatformIO virtual environment:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e arduino_nano_esp32_idf5
```

The resulting OTA image is
`.pio/build/arduino_nano_esp32_idf5/firmware.bin`. Do not substitute
NimBLE-Arduino 1.x in this environment; it is retained only for the IDF-4
recovery profiles.

`tools/upload_ota.ps1` allows ten minutes for the request body by default.
This is intentional: a production image sent through a Tailscale DERP relay
or another high-latency uplink can need more than the former three-minute
timeout. Override `-UploadTimeoutSeconds` only when the path is known. A client
timeout does not prove that the inactive image was activated; inspect
`GET /api/ota` and confirm the running partition before retrying.

Production-profile builds report the next real release version from
`FIRMWARE_VERSION`; MeshGate does not distribute `-dev` OTA images. Bump that
value before building an end-user image and create the identical SemVer Git
tag. The release workflow injects the tag again in its clean build, preventing
the published binary and release name from diverging.

The official updater treats release metadata and firmware transport failures
as transient on remote links. Metadata receives four bounded attempts. Once
flash writing starts, the firmware transfer resumes with HTTP Range requests
from the already hashed and written byte offset for up to twenty minutes.
Only flash-write, SHA-256, size, board, or slot-validation failures are fatal
immediately. Do not replace this elapsed-time retry window with a small fixed
connection count: unstable remote paths can require more than eight
resumptions for a single 1.9 MB image.

## 4. Flashing the ESP32
1. Plug your ESP32-S3 into your Mac via USB.
2. In the IntelliJ terminal, run:
```bash
pio run -target upload
```
*(Shortcut: `pio run -t upload`)*
PlatformIO will automatically find the correct USB/serial port on your Mac, compile the code, and upload the firmware.

The Nano uses the local `variants/arduino_nano_nora` definition and the
project's dual-slot 16 MB partition table. A first-time partition migration
must be flashed over USB. Later application updates should use the authenticated
OTA endpoint and `tools/upload_ota.ps1`, which verifies the image MD5 and the
active-partition change after reboot.

## 5. Serial Monitor
To view the `Serial.println()` output for debugging, you can use the built-in serial monitor:
```bash
pio device monitor
```
To exit the serial monitor, press `Ctrl + C`.

---
## Summary of Commands
* **Build:** `pio run`
* **Upload:** `pio run -t upload`
* **Monitor:** `pio device monitor`
* **Upload & Monitor:** `pio run -t upload -t monitor`

---
## 6. Hardware Configuration & Build Options
This project is designed to be highly portable across different ESP32 hardware variants. All magic numbers (like hardware pins, port numbers, and LED behaviors) are centralized in the `include/build_options.h` file.

### Overriding at Compile Time
You do **not** need to modify the C++ code to port this project to a new board. All constants defined in `build_options.h` are wrapped in `#ifndef` guards, meaning you can dynamically override them at compile-time directly inside the `platformio.ini` file using `build_flags`.

For example, if you are using a standard NodeMCU board where the physical BOOT button is on GPIO 15 and the LED is Active High, you would add this to your `platformio.ini`:

```ini
[env:custom_board]
platform = espressif32
board = nodemcu-32s
framework = arduino
build_flags = 
    -D BOOT_BUTTON_PIN=15
    -D LED_ACTIVE_LOW=false
```

### Available Build Flags
- `BOOT_BUTTON_PIN`: The GPIO pin for the physical button used to force Setup Mode (Default: `0`).
- `STATUS_LED_PIN`: The GPIO pin for the visual status LED (Default: `21`).
- `LED_ACTIVE_LOW`: Set to `true` if your board's LED turns ON when the pin is pulled LOW (e.g., XIAO ESP32S3). Set to `false` if it turns ON when pulled HIGH. (Default: `true`).
- `TCP_PORT`: The network port the bridge listens on for incoming Meshtastic App connections. (Default: `4403`).
- `TCP_IDLE_TIMEOUT_SECONDS`: How long before idle TCP connections will be disconnected. (Default: `600`).
- `MAX_TCP_CLIENTS`: The maximum number of simultaneous apps that can connect to the bridge. (Default: `3`).
- `BLUETOOTH_TIMEOUT_SECONDS`: Bluetooth connection timeout. (Default: `8`).
- `BLUETOOTH_SCAN_TIME_SECONDS`: Bluetooth discovery scan duration. (Default: `4`).
- `BLUETOOTH_MAX_DEVICES_DISCOVERABLE`: Maximum number of BLE devices to keep in memory from discovery scan. (Default: `60`).
- `WIFI_MAX_NETWORKS_DISCOVERABLE`: Maximum number of WiFi networks to keep in memory from discovery scan. (Default: `30`).
- `BRIDGE_QUEUE_SIZE`: Capacity of the bidirectional FreeRTOS queues between BLE and TCP tasks. (Default: `32`).
- `HTTP_PHONEAPI_QUEUE_SIZE`: Capacity of the dedicated BLE-to-HTTP polling queue. (Default: `8`).
- `HTTP_PHONEAPI_ACTIVE_TIMEOUT_MS`: HTTP client activity window before stale queued packets are cleared. (Default: `15000`).
- `MQTT_QUEUE_SIZE`: Capacity of the static MQTT downlink queue. (Default: `16`).

### Static FreeRTOS Queues & RAM Sizing Math
All FreeRTOS queues (`tcp_to_ble_queue`, `ble_to_tcp_queue`, `ble_to_http_queue`, and `mqtt_to_ble_queue`) are statically allocated in the `.bss` segment using `xQueueCreateStatic` to prevent runtime dynamic heap fragmentation and eliminate Out-Of-Memory crashes.

Each `BridgePacket` holds a 576-byte payload plus length metadata ($\approx 584\text{ bytes}$).

#### Unified Sane Sizing Across All ESP32 & ESP32-S3 Targets
* **Queue Allocations:**
  - `tcp_to_ble_queue`: $32 \text{ packets} \times 584\text{ B} = 18.7\text{ KB}$
  - `ble_to_tcp_queue`: $32 \text{ packets} \times 584\text{ B} = 18.7\text{ KB}$
  - `mqtt_to_ble_queue`: $16 \text{ packets} \times 584\text{ B} = 9.3\text{ KB}$
  - **Total Static Queue RAM:** $\approx 46.7\text{ KB}$ in `.bss`.
* **Classic ESP32 override:** `esp32dev` uses `BRIDGE_QUEUE_SIZE=24`. Its
  statically linkable DRAM window is smaller than ESP32-S3, and this retains
  24 complete packets per direction while leaving linker/runtime headroom for
  the persistent mailbox. S3 profiles retain the default 32 unless explicitly
  overridden below.
* **Runtime Free Heap Footprint:**
  - **Seeed XIAO ESP32-S3:** **~106 KB free heap** (Min: ~88 KB, MaxBlock: ~95 KB).
  - **Classic ESP32 (`esp32dev`):** **~95 KB free heap** (Min: ~78 KB, MaxBlock: ~80 KB).
* **Why This Matters:** LwIP networking, AsyncWebServer, and MbedTLS require ~15 KB of contiguous free memory. Sizing the queues to 32/16 ensures $>5\times$ safety margin on all targets without risking memory exhaustion or socket starvation.

#### IDF-5 / MicroLink queue sizing

The `arduino_nano_esp32_idf5` profile overrides `BRIDGE_QUEUE_SIZE=4`,
`MQTT_QUEUE_SIZE=2`, and `CONFIG_ML_MAX_PEERS=4`. It also places only the DERP
and coordination task stacks in PSRAM. This preserves a contiguous internal
block for the mbedTLS DERP context and enough runtime memory for TCP clients.
The network-I/O and WireGuard-manager stacks must remain internal because they
can execute during flash-cache-disabled operations; moving either to PSRAM
causes an immediate cache-safety assertion or IPC stack-canary panic. Do not
change these overrides without an on-device boot, DERP-handshake, OTA, BLE, and
three-client TCP stress test.

#### Production load-shedding baseline

On 2026-11-12 the deployed IDF-5 Nano was exercised with eight rounds across
five concurrent HTTP paths (`/status`, `/metrics`, `/api/logs`, the bounded
mailbox query and the local SVG), followed by 20 sequential TCP connections and
three simultaneous port-4403 clients. `/status` remained available for all
eight requests, all 20 sequential TCP connections and all three concurrent
clients succeeded, BLE and MQTTS stayed connected, and every bridge queue kept
its drop counter at zero. Under the first HTTP burst the historical minimum
heap reached 476 bytes. A follow-up with three sustained four-request batches
reached 184 bytes even though the device recovered to about 25 KiB free heap
without a restart. The low-memory admission threshold was therefore raised to
32 KiB free heap or a 16 KiB largest internal block, and low-memory mode now
admits only one ordinary response at a time while keeping two reserved recovery
slots. Treat resulting `503` responses as successful load shedding, not as
endpoint failure.
Future changes to HTTP admission control, asset serving, TLS allocation or task
stacks must repeat this test and preserve the always-available status/recovery
path. The production Wi-Fi RSSI during this baseline was between -93 and
-98 dBm, so latency figures from the run are not a clean LAN throughput
benchmark.

A controlled `/api/reboot` recovery check on the same deployment returned
`202`, produced an observable offline interval, and restored LAN HTTP plus TCP
4403 after 15.8 seconds. BLE, MQTTS, Tailscale control-plane registration and
the DERP data path were all ready after 20.2 seconds. The persistent mailbox
retained its three records and unchanged next-sequence cursor, all queue-drop
counters remained zero, and the fresh boot reported 33.6 KiB free heap with a
13.4 KiB observed minimum. Use these values as a regression baseline for
scheduled/manual restart work; a successful reboot must preserve both NVS
configuration and LittleFS mailbox state.

A later mixed-path test held two TCP 4403 clients open while repeatedly
churning the third slot and requesting LAN status, metrics, the local SVG and
Tailscale status concurrently. The device preserved BLE, MQTTS, Tailscale/DERP
and all queues without drops or a restart, but one four-request round exceeded
an 8-second client timeout. Internal heap temporarily reached 9.2 KiB free with
a 1.5 KiB largest block, then recovered without intervention to 24.6 KiB free
with a 7.7 KiB largest block. This is a diagnostic limit, not a throughput
target: future tests must treat multiple held PhoneAPI sockets plus simultaneous
LAN and DERP asset requests as the high-pressure case and confirm that reserved
status/recovery endpoints remain responsive before adding more load.

A follow-up immediately after the scheduled 18:00 maintenance restart held all
three TCP 4403 client slots while running six concurrent batches of LAN
`/status`, Tailscale `/status`, and alternating metrics, logs, mailbox and SVG
requests. All 18 requests returned `200`; both status paths succeeded in every
batch, and the slowest response took 6.5 seconds over the production -90 dBm
Wi-Fi link. The historical minimum heap reached 2.5 KiB but recovered to
32.6 KiB free with a 12.3 KiB largest block after the clients closed. BLE,
MQTTS and DERP remained connected, all queue-drop counters stayed at zero, and
the mailbox retained the same three records and sequence cursor. This confirms
correctness under full client-slot pressure, but the latency and minimum-heap
figures remain a stress boundary rather than a supported throughput target.

The stock ESPAsyncWebServer `AsyncResponseStream` is not safe for production
dynamic responses on this constrained profile. Its send path allocates the
complete current AsyncTCP send-window size on every acknowledgement. With
MQTT TLS, BLE and Tailscale/DERP active, total free heap can still look healthy
while the largest contiguous internal block is smaller than that window. The
allocation then fails on every retry and the response never emits even its
headers. All dynamic production responses therefore use
`BoundedStreamResponse`: the body retains the familiar `Print` construction
interface, but the completed body and headers are sent directly in
acknowledged 512-byte blocks without a send-time allocation. Do not reintroduce
`beginResponseStream()` on the production HTTP path. Any response-path change
must be tested with MQTT TLS and Tailscale connected and must repeatedly return
`/status`, `/config`, `/tailscale_status`, `/metrics`, `/api/mailbox` and both
PhoneAPI endpoints without an indefinitely pending request.

A subsequent 30-connection TCP churn test exposed a separate ownership leak:
all connections and 30 concurrent HTTP checks succeeded, but internal free heap
remained about 7.4 KiB below its pre-test baseline after more than three
minutes. `AsyncServer` creates every accepted `AsyncClient` with `new` and
transfers ownership to the application callback. MeshGate deleted its
per-client context on disconnect but not the `AsyncClient` itself. The
disconnect callback now releases both objects, and the max-client rejection
path installs the same deferred cleanup before closing. Both the IDF-5
production profile and Arduino-Core-2 recovery profile compile successfully
with the fix. Any future TCP churn regression must verify that free heap returns
to its pre-test range after lwIP connection cleanup, not merely that the active
client count returns to zero.

`GET /tailscale_status` must show both `connected: true` and
`derp_connected: true`. The first value alone only proves Tailscale control-plane
registration and does not prove that HTTP or TCP 4403 can traverse the tailnet.

The connected-device UI deliberately exposes two distinct values: the native
Meshtastic app uses `<tailscale-ip>:4403`, while the browser-based Meshtastic
Client uses the bare `<tailscale-ip>` with HTTP selected and HTTPS disabled.
Do not repeat the address in the lower status list. The firmware footer must
resolve `PROJECT_GITHUB_URL` to the MeshGate repository; attribution for
[SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge)
belongs in repository documentation rather than the product navigation.
When documentation refers to this original bridge project, use that complete
linked repository name instead of the ambiguous phrase “upstream bridge.”

The embedded control page uses four client-side tab panels (`connection`,
`remote`, `services`, and `system`). Cards remain mounted in the DOM while an
inactive panel is hidden, so live polling, edit state and write-only secret
placeholders survive tab changes. Deep links use the matching URL fragment.

`generate_web_ui.py` extracts that page during every PlatformIO build,
resolves its firmware-version and repository placeholders, and writes a
deterministic gzip asset to `web/generated/index_html.gz`. The firmware embeds
both forms in flash: modern browsers receive gzip while clients without gzip
support receive the original `PROGMEM` page. Do not add runtime compression or
construct a page-sized heap buffer; constrained Tailscale DERP paths depend on
the pre-compressed response remaining allocation-free.

GitHub Pages and the normal device control page share FlagCounter ID `kOa`.
The device must assign the external image URL only after `/status` proves that
station Wi-Fi is connected and setup AP mode is inactive. This preserves the
offline captive-portal contract and emits at most one country-counter request
per page load. Keep `referrerpolicy="no-referrer"` and the visible privacy note:
the external request exposes the visitor's public IP to flagcounter.me for
country-level geolocation. GitHub Pages remains static and stores no counter
state itself.

Interactive enrollment follows the native Tailscale registration protocol.
An unauthenticated RegisterResponse supplies `AuthURL`; MicroLink holds it in
RAM, exposes it through `microlink_get_auth_url()`, and sends it in the next
RegisterRequest as `Followup`. Receive timeouts during this control-plane long
poll mean approval is still pending, not registration success. Do not persist
the URL or replace this flow with copied OAuth/API credentials.

The vendored MicroLink WireGuard-interface initialization must retain its
TCPIP-core lock around raw lwIP `netif`/PCB registration. On ESP-IDF 5, removing
that lock produces `netif_set_up ... Required to lock TCPIP core functionality!`
on serial and an automatic reboot loop before Tailscale can register.

Inbound decrypted WireGuard packets from the MicroLink manager task must enter
lwIP through `netif->input` (`tcpip_input` in magicsock mode), never through a
direct `ip_input()` call. The direct call violates IDF-5 core locking and
reboots on the first Tailscale transport packet. WireGuard peers also start
without a direct endpoint: MapResponse addresses remain probe candidates until
a direct DISCO PONG validates one. This keeps both handshake and data replies
on DERP for mobile/carrier-NAT clients instead of leaking replies to a stale
UDP endpoint. Verify both rules with an external peer: the serial log must show
an inbound DATA packet without an assertion, a DERP-routed DATA reply, and a
stable session rather than repeated new handshakes.

### Task Prioritization & Multithreading Architecture
To prevent high-bandwidth network traffic from starving BLE operations or locking up the WebUI:

| Task Name | Core | Priority | Stack Size | Notes |
| :--- | :---: | :---: | :---: | :--- |
| **`LwIP / TCP Stack`** | Core 0 | **18** | System | Internal ESP-IDF IP/MAC networking stack |
| **`AsyncTCP Worker`** | Core 0 | **3** | System | Handles async socket read/write events and WebUI endpoints |
| **`mqtt_client` Task** | Core 0 | **2** | 8192 B | Background MQTT networking and TLS processing |
| **`bridgeNetTask`** | Core 0 | **1** | 4096 B | Network broadcast task (drains `ble_to_tcp_queue` to TCP sockets) |
| **`diag_telemetry`** | Core 0 | **1** | 3072 B | Periodic diagnostic task (15s interval) |
| **`bridgeBleTask`** | Core 1 | **1** | 8192 B | BLE Engine (polls `FromRadio`, handles `FromNum`, writes to `ToRadio`) |
| **`reliability_task`** | Core 1 | **2** | 4096 B | Watchdog, link supervision and allocation-free restart requests |

The BLE task always services app/TCP `ToRadio` traffic before MQTT downlink
traffic. After a lightweight top-level wire-field filter, it copies mailbox
candidates into a statically allocated four-entry queue; `bridgeNetTask`
performs protobuf decoding and LittleFS writes on Core 0
only after live PhoneAPI delivery and replay. This deliberately allows
Web/WLAN work to lag instead of blocking BLE. Outgoing mailbox history is
queued only after a successful BLE characteristic write; failed writes are
never recorded. The mailbox replay path cannot enqueue data into `ToRadio`, so
replay cannot duplicate an LoRa transmission. The record's existing reserved
byte stores sent/received direction without changing the version-1 file size
or invalidating retained mailbox data.

The reliability supervisor suppresses scheduled, link-failure, low-memory and
manual restart requests while OTA owns the update path. Only the successful OTA
completion path may request its final reboot, after the complete inactive-slot
image has passed size and digest validation and `Update.end()` has activated it.

---
## 7. Logging & Debugging

This project uses a combination of native ESP-IDF logging macros and custom overrides to maintain a clean console while allowing dynamic runtime debugging.

### Background & Limitations
The underlying Arduino Core (specifically version 2.0.17 used in the official `6.9.0` platform) maps standard `log_d()` calls to a raw `log_printf`. This bypasses the native ESP-IDF `esp_log_level_set()` filtering mechanism. If we compile with global debug logs enabled (`CORE_DEBUG_LEVEL=4`), the internal Arduino libraries (like the WiFi driver) will constantly spam the console with their internal state machine events (`WIFI_READY`, `STA_START`, etc.), and we cannot silence them at runtime.

### Our Solution
To achieve a clean console while preserving our own debug logs, we use the following strategy:

1. **Global Compile-Time Mute:** In `platformio.ini`, we set `-D CORE_DEBUG_LEVEL=3` (Info Level). This instructs the compiler to permanently strip out all `log_d` (Debug) and `log_v` (Verbose) statements from the background Arduino libraries, completely silencing the WiFi and OS spam.
2. **Runtime Toggle for App Logs:** We want our own debug logs to be toggleable via the Web UI. Since the global `CORE_DEBUG_LEVEL=3` would normally strip our `log_d` calls too, we bypass it. In `include/utils.h`, we `#undef log_d` and redefine it as our own custom macro:
   ```cpp
   #define log_d(format, ...) do { if(g_debug_logs) log_printf(ARDUHAL_LOG_FORMAT(D, format), ##__VA_ARGS__); } while(0)
   ```
   This ensures that any of *our* source files that include `utils.h` can use standard `log_d("...")` syntax, but the logs will only print if the user has enabled the "Enable Serial Debug Logs" toggle in the Web UI.

### Adjusting Logs
- **Application Logs (Normal Use):** Simply connect to the Web UI (Setup Mode) and toggle **"Enable Serial Debug Logs"**. This sets `g_debug_logs = true` and saves it to NVS.
- **Deep Framework Debugging (Advanced):** If you absolutely must debug the internal state machines of the WiFi or LwIP libraries, you must modify `platformio.ini`, set `-D CORE_DEBUG_LEVEL=4`, and recompile the firmware. Be prepared for significant console noise.
- **Bluetooth (NimBLE) Logs:** NimBLE is configured via its own flag in `platformio.ini`. We currently set `-D CONFIG_NIMBLE_CPP_LOG_LEVEL=2` (Warning) to keep it quiet. You can increase this to `4` (Debug) if you need to debug raw GATT characteristics.

---
## 8. TLS Root Certificate Authority (CA) Bundle Pipeline

To support secure MQTTS connections to any public broker (such as `mqtt.meshtastic.org:8883` using Let's Encrypt, or AWS IoT, HiveMQ, EMQX, etc.) without hardcoding static server certificates, the project embeds a compact, pre-compiled Mozilla Root CA bundle.

### How it Works
1. **Pre-Compiled Bundle (`data/cert/x509_crt_bundle.bin`):** Contains ~130+ standard trusted root CA certificates compressed into a binary format (subject names + public keys only, ~86 KB total).
2. **PlatformIO Embedding:** Configured in `platformio.ini` together with the
   shared MeshGate logo via:
   ```ini
   board_build.embed_files =
       data/cert/x509_crt_bundle.bin
       web/assets/meshgate-logo.svg
   ```
   During compilation, the linker exposes the binary start symbol `_binary_data_cert_x509_crt_bundle_bin_start`.
3. **Runtime Initialization:** On boot, `mqtt_net_init()` invokes:
   ```cpp
   extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");
   arduino_esp_crt_bundle_set(rootca_crt_bundle_start);
   ```
4. **Binary Search Verification:** When a TLS connection is opened, mbedTLS uses binary search against the embedded flash memory to verify server certificate chains with minimal RAM overhead.
5. **Self-Signed / Insecure Bypass:** When the "Skip Certificate Validation" toggle is enabled in the WebUI, the bridge attaches a custom handler that sets `MBEDTLS_SSL_VERIFY_NONE` and skips hostname verification, allowing local LAN brokers and self-signed certificates.

### Updating the CA Bundle
Root CA certificates change very infrequently (with 15–30 year validity windows), but the bundle can be refreshed at any time directly from the official Mozilla / cURL certificate store:

```bash
# 1. Create a temporary Python virtual environment with cryptography
python3 -m venv /tmp/ca_env
/tmp/ca_env/bin/pip install cryptography --quiet

# 2. Download Espressif's standalone bundle generator and the latest Mozilla CA store
curl -s https://raw.githubusercontent.com/espressif/esp-idf/release/v5.1/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py -o /tmp/gen_crt_bundle.py
curl -s https://curl.se/ca/cacert.pem -o /tmp/cacert.pem

# 3. Generate the compact binary bundle and move to data/cert/
/tmp/ca_env/bin/python /tmp/gen_crt_bundle.py -i /tmp/cacert.pem
mv x509_crt_bundle data/cert/x509_crt_bundle.bin

# 4. Clean up temporary files
rm -rf /tmp/ca_env /tmp/gen_crt_bundle.py /tmp/cacert.pem
```

*Note: Make sure to commit the updated `data/cert/x509_crt_bundle.bin` to Git so CI/CD and other developers compile with the new certificate store.*

### Shared MeshGate brand asset

`web/assets/meshgate-logo.svg` is the canonical transparent vector logo. The README,
GitHub Pages Web Flasher and embedded device UI must reference that same file;
do not create divergent copies or redraw it. The linker exposes the embedded
asset as `_binary_web_assets_meshgate_logo_svg_start` and `_end`, and the
device serves it at `/assets/meshgate-logo.svg`. Keep this route local so the
captive portal remains usable without internet access. Keep the committed SVG
at or below 10 KiB and include its intrinsic width and height in both HTML views.
Large image responses can occupy the constrained AsyncTCP/Tailscale send path
long enough to delay `/status` and Meshtastic clients after an interrupted
mobile download. Change the firmware-version query string whenever the asset
changes so browser caches cannot retain a failed older response.

---
## 9. Releasing a New Version

The project is fully automated using GitHub Actions. To release a new firmware version, you **do not** need to manually compile or upload binaries. 

Follow these steps to trigger the CI/CD pipeline:

1. **Commit your code to `main`**: Ensure all your local changes are pushed to the `main` branch.
2. **Set the release version**: Update `FIRMWARE_VERSION` in
   `include/build_options.h` to the exact SemVer version being released.
3. **Write the technical release entry**: Add a matching newest-first
   `## [vX.Y.Z]` section to `CHANGELOG.md`. Record firmware, runtime, security,
   compatibility, API, build and release-pipeline changes only. Do not include
   promotional or documentation-only edits. The release job fails if the
   matching entry is missing and uses that entry as the GitHub Release body.
4. **Tag the release**: Create the identical SemVer Git tag (e.g., `v1.2.0`) and push it to GitHub:
   ```bash
   git tag v1.2.0
   git push origin v1.2.0
   ```
5. **Wait for the Release Pipeline**:
   * Navigate to the **Actions** tab on your GitHub repository.
   * The **Release Firmware** workflow injects the tag into the firmware, builds the Arduino Nano ESP32 IDF-5 production profile, publishes its ZIP, OTA/factory binaries and SHA-256 checksums, and creates a public GitHub Release.
6. **Wait for the Web Flasher Pipeline**:
   * Successful completion of the release workflow automatically triggers **Deploy Web Flasher to GitHub Pages**. A `workflow_run` trigger is used because releases created with `GITHUB_TOKEN` do not emit another ordinary workflow event.
   * It will securely download the release binaries, automatically generate the `manifest.json` configurations for ESP Web Tools, and deploy them to `https://p1337.github.io/meshgate/`.
   * It also creates `latest_ota.json` with the production OTA image URL, exact
     byte length and SHA-256 digest. The on-device one-click updater consumes
     only this fixed official channel; publish Pages before testing an OTA.
   * The web flasher will instantly update to show "Firmware Version: v1.2.0".

### License-complete release assets

The combined firmware is distributed under `GPL-3.0-only`. The release job
must therefore continue to publish `meshgate-<version>-source.zip` next to
every binary. GitHub's automatic source archives are insufficient on their own
because they contain only the `proto/meshtastic` Gitlink, not the checked-out
GPLv3 Protocol Buffer sources used by the build. The workflow creates the
source archive from `git archive` for both the parent repository and the exact
submodule revision, then includes that archive in `SHA256SUMS.txt`.

Do not remove the root `LICENSE`, `LICENSE_POLICY.md`,
`THIRD_PARTY_NOTICES.md`, `LICENSES/`, component license files, build scripts,
partition data or the protobuf source tree from that archive. Those files are
part of the corresponding source and provenance required for a redistributable
binary release.
