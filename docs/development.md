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
- `MQTT_QUEUE_SIZE`: Capacity of the static MQTT downlink queue. (Default: `16`).

### Static FreeRTOS Queues & RAM Sizing Math
All FreeRTOS queues (`tcp_to_ble_queue`, `ble_to_tcp_queue`, and `mqtt_to_ble_queue`) are statically allocated in the `.bss` segment using `xQueueCreateStatic` to prevent runtime dynamic heap fragmentation and eliminate Out-Of-Memory crashes.

Each `BridgePacket` holds a 576-byte payload plus length metadata ($\approx 584\text{ bytes}$).

#### Unified Sane Sizing Across All ESP32 & ESP32-S3 Targets
* **Queue Allocations:**
  - `tcp_to_ble_queue`: $32 \text{ packets} \times 584\text{ B} = 18.7\text{ KB}$
  - `ble_to_tcp_queue`: $32 \text{ packets} \times 584\text{ B} = 18.7\text{ KB}$
  - `mqtt_to_ble_queue`: $16 \text{ packets} \times 584\text{ B} = 9.3\text{ KB}$
  - **Total Static Queue RAM:** $\approx 46.7\text{ KB}$ in `.bss`.
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

`GET /tailscale_status` must show both `connected: true` and
`derp_connected: true`. The first value alone only proves Tailscale control-plane
registration and does not prove that HTTP or TCP 4403 can traverse the tailnet.

The vendored MicroLink WireGuard-interface initialization must retain its
TCPIP-core lock around raw lwIP `netif`/PCB registration. On ESP-IDF 5, removing
that lock produces `netif_set_up ... Required to lock TCPIP core functionality!`
on serial and an automatic reboot loop before Tailscale can register.

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
2. **PlatformIO Embedding:** Configured in `platformio.ini` via:
   ```ini
   board_build.embed_files = data/cert/x509_crt_bundle.bin
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

---
## 9. Releasing a New Version

The project is fully automated using GitHub Actions. To release a new firmware version, you **do not** need to manually compile or upload binaries. 

Follow these steps to trigger the CI/CD pipeline:

1. **Commit your code to `main`**: Ensure all your local changes are pushed to the `main` branch.
2. **Tag the release**: Create a SemVer-compliant git tag (e.g., `v1.2.0`) and push it to GitHub:
   ```bash
   git tag v1.2.0
   git push origin v1.2.0
   ```
3. **Wait for the Release Pipeline**: 
   * Navigate to the **Actions** tab on your GitHub repository.
   * You will see the **Draft Release** workflow running. This workflow dynamically injects the `v1.2.0` string into the C++ code, compiles all hardware profiles (`esp32dev`, `esp32s3`, etc.), packages the `.bin` files, and drafts a GitHub Release for you.
4. **Publish the Draft**:
   * Navigate to the **Releases** tab on your GitHub repository.
   * Click **Edit** on the newly generated Draft release.
   * Add any specific release notes and click **Publish Release**.
5. **Wait for the Web Flasher Pipeline**:
   * As soon as you click Publish, the **Deploy Web Flasher to GitHub Pages** action will automatically trigger.
   * It will securely download your newly compiled release binaries, automatically generate the `manifest.json` configurations for ESP Web Tools, and deploy them to `https://sourcelaborg.github.io/meshtastic-esp32-bt-bridge/`.
   * The web flasher will instantly update to show "Firmware Version: v1.2.0".
