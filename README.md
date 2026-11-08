# Meshtastic ESP32 BLE-to-TCP Bridge

> **Development status:** The production `arduino_nano_esp32` target currently
> builds with Arduino Core 2.0.17 / ESP-IDF 4.4.7. The separate
> `arduino_nano_esp32_idf5` environment is the active migration target for
> Arduino Core 3.3.9 / ESP-IDF 5.5.4 and future MicroLink/Tailscale support. Do
> not treat that migration target as production-ready until its build and
> on-device tests are documented as complete.

A simple, lightweight ESP32 firmware that bridges a Meshtastic radio's Bluetooth connection to your local WiFi network. 

If you have a Bluetooth-only Meshtastic device (like a T-Echo, SenseCAP, or a base station without WiFi) and want to connect to it over your home network using the Meshtastic Web UI, MeshMonitor, or the mobile apps, this bridge acts as a transparent middleman. It connects to your radio via Bluetooth Low Energy (BLE) and exposes it as a standard Meshtastic network node on TCP port 4403.

> **Note:** This project is heavily inspired by the Python-based [meshtastic-ble-bridge](https://github.com/Yeraze/meshtastic-ble-bridge). If you are looking to run a bridge on a Raspberry Pi or another Linux computer rather than dedicated microcontroller hardware, use the Linux bridge project instead.

## How It Works

Once flashed to an ESP32, the bridge operates entirely standalone:

1. **Setup Mode:** On first boot, the ESP32 acts as its own WiFi Access Point. You connect to it with your phone or laptop to access a simple Web UI.
2. **Configuration:** In the Web UI, you provide your local home WiFi credentials, and scan for your Meshtastic radio's Bluetooth MAC address and PIN.
3. **Bridge Mode:** The ESP32 reboots, connects to your home WiFi, pairs securely with your Meshtastic radio over Bluetooth, and quietly runs in the background. 
4. **Auto-Discovery:** The bridge broadcasts itself on your local network using mDNS. Official Meshtastic apps will automatically discover it as if the radio itself was directly plugged into your router.

## What Hardware do I Need?

You will need a supported ESP32 microcontroller board. 

**Highly Recommended:**
*   **[Seeed Studio XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html)**: This is the officially recommended board for this project. It is incredibly tiny, has native USB support, and features a powerful antenna.
    *   *Optional:* You can **[3D print this case](https://www.printables.com/model/1445678-case-for-seeed-xiao-esp32s3-and-the-default-antenn/files)** designed specifically to hold the board and its default antenna!

**Other Supported Hardware:**
*   **Generic ESP32-S3 DevKit:** Any standard ESP32-S3 development board with native USB.
*   **Generic ESP32 (Classic):** Standard WROOM-32 or NodeMCU style boards.

*(Note: ESP32-S2 boards are not supported as they lack Bluetooth hardware).*

## How Do I Install It?

The absolute easiest way to install this firmware is by using our zero-install Web Flasher. You don't need to download any tools or compilers.

1. Plug your ESP32 board into your computer via USB.
2. Open a supported browser (Chrome, Edge, or Opera on a desktop OS).
3. **[Launch the Meshtastic Bridge Web Flasher](https://sourcelaborg.github.io/meshtastic-esp32-bt-bridge/)**
4. Locate your hardware in the list.
5. If this is a brand new board, click **Factory Reset (Wipes Data)**. If you are upgrading an existing bridge, click **Update Firmware**.

## How do I set it up?

Once you have successfully flashed the firmware to your ESP32, follow these steps to configure it:

1. Power on your ESP32. The built-in LED will blink rapidly for 5 seconds.
2. The LED will change to a repeating setup pattern (short, short, long, long). This indicates it is broadcasting its Setup WiFi network.
3. On your phone or computer, connect to the WiFi network named **`Meshtastic-Bridge-Setup`**.
4. A Captive Portal should automatically appear (if it doesn't, navigate to `http://192.168.4.1` in your browser).
5. Enter your home WiFi credentials.
   Saving WiFi starts the station connection immediately while the setup AP
   remains online. As soon as DHCP assigns an address, the web UI is reachable
   through both `192.168.4.1` and the new LAN IP, and the Nano displays the LAN
   IP's final octet once on its RGB LED.
6. Scan for your target Meshtastic device and enter its 6-digit Bluetooth PIN. (You can click "Test Connection" to verify it works).
   The scanner intentionally shows only advertisements containing the official
   Meshtastic BLE service UUID; unrelated rings, watches, and anonymous BLE
   beacons are hidden. A valid radio can appear as `Meshtastic device` when its
   advertisement omits the friendly name. If no radio appears, disconnect it
   from any phone, wait ten seconds, and scan again because a connected radio
   normally stops advertising to other BLE clients.
7. Click **Reboot & Start Bridge**.

During normal operation the bridge automatically recreates a timed-out BLE
client and rediscovers the configured radio by the official Meshtastic service
UUID and advertised name. This avoids permanent outages when an nRF52 radio
changes its private BLE address after a reboot. If exactly one Meshtastic radio
is visible, it can also be recovered when the advertisement omits its name.
Before encrypted GATT discovery begins, the bridge waits for the nRF52 pairing
callback and verifies that the link is encrypted. It never reports the BLE
bridge as ready unless all required Meshtastic characteristics were discovered.

The ESP32 will reboot, and its LED will turn **solid** once it has successfully connected to both your WiFi network and your Meshtastic radio.

## How do I use it?

Once the bridge is running with a solid LED, it is entirely transparent!

1. Open your Meshtastic App (iOS, Android, or Web UI).
2. Select a **Network / TCP** connection.
3. The bridge will automatically advertise itself on your local network using Apple Bonjour / mDNS. It will show up using the exact same name as your radio's Bluetooth name (e.g., `Meshtastic_ABCD`).
4. Simply click to connect! 

> **Note:** The bridge supports multiplexing, meaning **up to 3 clients** (e.g., your phone, your tablet, and your desktop) can all connect to the radio simultaneously over WiFi!

### MQTT Gateway / Proxy (Optional)

In addition to serving local TCP app clients, the bridge can act as an independent **MQTT Gateway/Proxy** for your Bluetooth Meshtastic device.

If the MQTT Gateway feature is enabled and MQTT is enabled on your radio (**MQTT Enabled** and **Proxy to Client Enabled** in the Meshtastic app), 
the bridge automatically discovers your broker address, port, credentials, root topic (`msh/US`, `msh/JP`, etc.), and TLS configuration over 
Bluetooth and establishes the connection.  It will then proxy MQTT messages between its MQTT client connection and your radio using
your WiFi connection.

To prevent duplicate messages, the bridge automatically filters out MQTT packets from reaching your TCP clients to prevent sending
duplicate messages to the MQTT server.

**Disconnect Grace Period:** If the Bluetooth connection drops momentarily, the bridge keeps the MQTT broker connection alive for up 
to 60 seconds and buffers incoming messages to avoid losing messages.

### Arduino Nano ESP32, WireGuard, and unattended operation

This deployment also supports the Arduino Nano ESP32 (ESP32-S3) through the
`arduino_nano_esp32` PlatformIO environment. GPIO0/B1 remains reserved for ROM
download mode; the Nano build therefore does not treat it as a setup button or
status LED. To enter ROM download mode, short B1 to GND, press RST once, then
remove the short. Uploads use `esptool` rather than the Windows DFU interface.

After WiFi connects, the Nano's RGB LED announces the final octet of its local
IPv4 address exactly once. The hundreds digit blinks red, the tens digit blinks
green, and the ones digit blinks blue, with a fixed pause between groups. For
example, `192.168.0.124` is shown as one red, two green, and four blue flashes.
The LED then remains off for normal operation. A zero digit produces no flashes
in its color group. The sequence runs again after each reboot because DHCP may
have assigned a different address.

The setup portal contains an optional WireGuard client. It supports an endpoint
hostname/IP, endpoint port, tunnel address, one remote IPv4 CIDR, public/private
keys, an optional preshared key, and a 25-second persistent keepalive. The
private and preshared keys are write-only: the HTTP configuration API reports
only whether they are present. The tunnel is a split route; normal internet
traffic remains on WiFi while traffic for the configured remote CIDR uses
WireGuard. The tunnel IP must be inside that remote CIDR so lwIP can route
replies through the WireGuard interface.

The Torun deployment temporarily reuses the exported Windows WireGuard peer
`wg_config` with tunnel address `192.168.1.202`. The Windows tunnel must remain
inactive while the ESP32 bridge is online because both endpoints otherwise use
the same WireGuard identity and tunnel address. Replace this temporary setup
with a dedicated FRITZ!Box peer when physical confirmation at the router is
available.

mDNS discovery is link-local and does not cross the routed WireGuard tunnel.
Remote Meshtastic clients must therefore use a manual TCP connection to
`192.168.1.202:4403`; the bridge will not automatically appear in discovery.
The bridge reporting WireGuard as connected proves that its peer handshake is
alive, but a second VPN client still depends on the FRITZ!Box allowing traffic
between its VPN peers. If the manual address is unreachable, that FRITZ!Box
peer-to-peer route is the next item to verify.

For unattended operation, configuration is stored in both `bridge_cfg` and the
redundant `bridge_backup` NVS namespaces. A missing primary configuration is
restored automatically at boot. Only the explicit **Reset All Settings** action
clears both copies. The reliability supervisor uses the task watchdog, restarts
after three minutes without WiFi or fifteen minutes without BLE/WireGuard, and
performs maintenance restarts at 00:00, 06:00, 12:00, and 18:00 local CET/CEST.
If time synchronization is unavailable for six hours, it performs one fallback
maintenance restart. Normal reboots never erase configuration.

The serial-style runtime log is also retained in a bounded 16 KiB RAM ring for
remote diagnostics. `GET http://<bridge-ip>/api/logs` returns the plain-text
log, while `POST http://<bridge-ip>/api/reboot` performs a controlled restart.
These endpoints are intended for automated support rather than an end-user UI.
Arduino `log_*` output and ESP-IDF log output are both captured; serial output
remains enabled whenever a USB CDC host is attached. With no serial host, the
Nano skips the potentially blocking USB write while continuing to record the
API log. Native USB CDC also uses a one-millisecond transmit timeout, so a
Windows driver that is enumerated but has no open terminal can never delay the
unattended network startup; serial output is best-effort in that state. Pairing
PIN values are deliberately redacted from both serial and API logs. Reboot
requests return HTTP 202 immediately, then restart one second later so remote
automation receives a reliable acknowledgement.

### Authenticated OTA updates (Arduino Nano ESP32)

The Nano profile uses the 16 MB dual-slot partition table (`app0`/`app1`) and
supports authenticated firmware uploads over ordinary HTTP. The initial
partition-table migration must be flashed once over USB; all later application
updates can use WiFi locally or the WireGuard address remotely. NVS starts at
the same offset in both partition layouts, so WiFi, BLE, and WireGuard settings
survive that migration and normal OTA updates.

`GET http://<bridge-ip>/api/ota` reports OTA readiness, the active and next
partition, and the maximum image size. `POST http://<bridge-ip>/api/ota`
accepts the raw `firmware.bin` body only when the private `X-OTA-Key` header is
valid. `X-Firmware-MD5` is also required by the supplied uploader so the image
is verified before activation. The token is stored redundantly in NVS and in
the local, git-ignored `.ota-token` deployment file; it is never returned by an
HTTP endpoint or written to logs.

From the project directory, the supported uploader is:

```powershell
pwsh tools/upload_ota.ps1 -Address 192.168.0.217 -Firmware .pio/build/arduino_nano_esp32/firmware.bin
```

For a remote update through the German FRITZ!Box, replace the address with
`192.168.1.202`. The script checks the image size, sends its MD5, waits through
the reboot, and confirms that the active OTA partition changed.

### Re-configuring the Bridge

If you ever need to change your WiFi credentials or connect to a different Meshtastic radio, you have two options:

**Option A (Via your local network):**
If the bridge is already connected to your home WiFi, simply open a web browser and navigate to `http://<your_device_name>.local` (e.g., `http://Meshtastic_ABCD.local`). This will load the Captive Portal UI directly over your home network.

**Option B (Hardware Reset):**
If you changed your WiFi router and the bridge can no longer connect to your network, you can force it back into Setup Mode:
1. Press the Reset (RST) button on your ESP32.
2. The LED will begin flashing rapidly for 5 seconds.
3. While it is flashing, press and hold the **BOOT** button on the ESP32.
4. The LED will switch to the setup pattern, and it will begin broadcasting the `Meshtastic-Bridge-Setup` WiFi Access Point again.
