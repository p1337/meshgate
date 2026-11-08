# Hackaday Project Draft — MeshGate

## Project title

MeshGate: Remote Meshtastic Administration from an ESP32-S3

## One-line pitch

An ESP32-S3 turns a Bluetooth-only Meshtastic radio into a secure, remotely
reachable TCP node using Wi-Fi, BLE and native Tailscale — no Raspberry Pi,
public IP or port forwarding required.

## The problem

Solar Meshtastic nodes are easy to leave on a roof, mast or remote property.
They are much harder to administer after you travel home. Many efficient radio
boards have Bluetooth but no Wi-Fi, and Bluetooth ends at the edge of the site.
Opening Meshtastic's TCP port directly to the internet is not a safe answer;
cellular clients and residential routers also sit behind NAT, and public IP
addresses change.

MeshGate places an ESP32-S3 within BLE range of the radio. It connects to the
radio's encrypted Meshtastic GATT service, reproduces the native PhoneAPI on
TCP port 4403, and joins a private Tailscale network with an outbound
connection. A phone on mobile data can then administer the remote radio as if
it were a normal network-connected Meshtastic node.

## What it does

- pairs with a Meshtastic radio using its six-digit BLE PIN;
- exposes the original protobuf stream on TCP port 4403;
- supports three simultaneous local or remote TCP clients;
- joins Tailscale directly on the Arduino Nano ESP32 production build;
- works through DERP when direct UDP cannot cross carrier NAT;
- imports MQTT broker settings from the radio and acts as its MQTT proxy;
- provides authenticated OTA, remote logs, JSON diagnostics and Prometheus
  metrics;
- preserves configuration across reboots and OTA with redundant NVS storage;
- recovers from Wi-Fi, BLE and VPN outages with watchdog logic;
- displays its DHCP address's final octet on the Nano RGB LED at boot.

## Architecture

```text
             BLE GATT                       Wi-Fi / Tailscale
┌──────────────────────────┐       ┌──────────────────────────────┐
│ Bluetooth Meshtastic node│◄─────►│ ESP32-S3 MeshGate           │
│ solar / battery powered  │       │ PhoneAPI TCP 4403           │
└──────────────────────────┘       │ Web UI · OTA · MQTT · logs  │
                                   └──────────────┬───────────────┘
                                                  │ encrypted tailnet
                                                  ▼
                                      iOS / Android / desktop app
```

## The difficult part

Getting a Tailscale machine to appear in the admin console was only the control
plane. The first real mobile-data connection exposed two data-plane defects.

First, decrypted WireGuard packets arrived in a manager task and called lwIP's
input path outside its TCPIP thread. ESP-IDF 5 correctly asserted on that core
lock violation. MeshGate now queues packet delivery to the lwIP TCPIP thread.

Second, the control plane supplied a possible carrier-network UDP endpoint.
Treating that candidate as proven caused ESP32 replies to leave through a path
that the phone could not receive. MeshGate keeps DERP as the fallback until
DISCO has validated a direct path. After both fixes, an iPhone on 5G reached
the setup page and Meshtastic TCP connection through DERP.

## The ESP-IDF 5 fork

MeshGate is based on
[SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge)
and retains its MIT license. This fork adds an Arduino Core 3.3.9 / ESP-IDF
5.5.4 production profile, NimBLE-Arduino 2.5.2, MicroLink/Tailscale, the data
plane fixes above, secure OTA and unattended-operation features. The original
ESP-IDF 4 Nano build remains as a recovery target.

## Hardware

The verified remote build uses:

- Arduino Nano ESP32 (ESP32-S3 with PSRAM)
- a USB power supply
- a Bluetooth-capable Meshtastic radio within BLE range
- ordinary Wi-Fi internet access

Other build targets cover the Seeed XIAO ESP32-S3, a generic ESP32-S3 DevKitC
and classic ESP32-WROOM/NodeMCU boards. Native Tailscale is currently enabled
only for the Nano IDF 5 profile.

## Build and first run

```powershell
git clone https://github.com/p1337/meshgate.git
cd meshgate
pio run -e arduino_nano_esp32_idf5
```

Flash with the Web Flasher or PlatformIO. Join the temporary
`Meshtastic-Bridge-Setup` access point, configure Wi-Fi and BLE, test the radio,
then enable Tailscale with a one-time auth key. After reboot, use the displayed
`100.x` address for the control UI and TCP port 4403.

## MQTT and the public map

The firmware includes MeshRadar's publicly documented shared uplink preset:
`meshradar.pl:8883`, `meshradar` / `meshradar`, TLS on, root `msh`. A complete
custom MQTT configuration from the radio replaces that standard, so the same image
also works with a private broker without compiling private credentials. Channel
uplink and position reporting remain explicit radio settings.

## Security model

The Tailscale auth key, BLE PIN, Wi-Fi password, WireGuard private keys and OTA
token are never returned by the configuration API. TCP 4403 and OTA are meant
for a private LAN or authenticated VPN, never direct public-internet exposure.
No deployment credentials belong in the repository.

## Current result

The production firmware has completed an end-to-end remote test: an iPhone on
mobile data opened the ESP32 control page and connected the Meshtastic app to
the remote radio through Tailscale DERP. The same Nano simultaneously held a
verified TLS connection to `meshradar.pl:8883` and subscribed to `msh/#`.
MQTT remains an independent, user-disableable data path rather than a
prerequisite for remote administration.

## Future work

- qualify native Tailscale on more ESP32-S3 boards;
- add signed release images in addition to token-authenticated OTA;
- expose richer radio health dashboards without duplicating private message
  content;
- test direct peer-to-peer UDP paths across more mobile carriers.

## Suggested Hackaday tags

`ESP32-S3`, `Meshtastic`, `LoRa`, `Tailscale`, `Bluetooth LE`, `VPN`, `IoT`,
`remote administration`, `PlatformIO`, `ESP-IDF 5`

## Suggested media

1. Hero photo: Nano ESP32 beside the target Meshtastic radio.
2. Architecture diagram showing BLE, Wi-Fi, Tailscale and TCP 4403.
3. MeshGate control page with connected BLE and Tailscale status.
4. iPhone on mobile data connected to the remote TCP address.
5. Serial or remote-log excerpt showing DERP ready and TCP client connected.
6. Web Flasher hardware-selection page.
