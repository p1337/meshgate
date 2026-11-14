<p align="center">
  <img src="web/assets/meshgate-logo.svg" alt="MeshGate" width="680">
</p>

<p align="center">
  <strong>Secure network access for Bluetooth-only Meshtastic radios.</strong><br>
  Keep the low-power radio outdoors. Run Wi-Fi, VPN, MQTT and remote operations on a powered ESP32-S3 indoors.
</p>

<p align="center">
  <a href="https://github.com/p1337/meshgate/actions/workflows/pr_check.yml"><img alt="Build Check" src="https://github.com/p1337/meshgate/actions/workflows/pr_check.yml/badge.svg"></a>
  <a href="https://github.com/p1337/meshgate/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/p1337/meshgate?display_name=tag&sort=semver"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-blue.svg"></a>
  <a href="https://p1337.github.io/meshgate/"><img alt="Install with Web Flasher" src="https://img.shields.io/badge/install-Web%20Flasher-00e88a"></a>
</p>

> [!IMPORTANT]
> **Project status: public beta.** The production path is actively tested on the
> **Arduino Nano ESP32 (ESP32-S3)**. It is the only supported build target.
> Review [Validation & Reliability](docs/validation.md)
> and [Known limitations](#known-limitations) before an unattended deployment.

## What MeshGate does

MeshGate connects to a Meshtastic radio over BLE and exposes the native
Meshtastic PhoneAPI over TCP `4403` and HTTP. Existing Meshtastic applications
can therefore use a Bluetooth-only radio as a network-connected node on the
local LAN or through an authenticated private network.

The primary deployment model is deliberately split:

- an energy-efficient Meshtastic radio remains outdoors on battery or solar;
- a mains-powered ESP32-S3 remains indoors and handles BLE, Wi-Fi and services;
- Tailscale or WireGuard provides remote access without public ports, dynamic
  DNS or an additional Raspberry Pi.

![MeshGate system architecture: solar Meshtastic radio over BLE to an indoor ESP32-S3 gateway with Wi-Fi, VPN, MQTT and message services](web/assets/meshgate-architecture.png)

<p align="center">
  <a href="https://p1337.github.io/meshgate/"><strong>Install MeshGate</strong></a>
  ·
  <a href="https://github.com/p1337/meshgate/releases/latest"><strong>Latest release</strong></a>
  ·
  <a href="docs/user_guide.md"><strong>User guide</strong></a>
  ·
  <a href="CHANGELOG.md"><strong>Changelog</strong></a>
</p>

## Highlights

- Native Meshtastic TCP `4403` and HTTP PhoneAPI transport
- Keyless, browser-approved Tailscale enrollment with DERP fallback
- Optional classic WireGuard and radio-managed MQTT proxying
- Persistent 128-message mailbox with received and sent conversation replay
- Built-in channel chat with unread counters and radio-accepted send status
- Verified dual-slot OTA with automatic stable updates and an admin-only beta channel
- Redundant configuration storage, watchdog recovery and scheduled restarts
- Remote status, bounded logs and Prometheus/OpenMetrics diagnostics
- Self-contained setup portal with no cloud dependency during provisioning

MeshGate transports the original Meshtastic protobuf stream. It does not
decrypt private channels, replace the Meshtastic protocol or retransmit mailbox
history over LoRa.

## Quick start

### 1. Install

Open the [MeshGate Web Flasher](https://p1337.github.io/meshgate/) in Chrome,
Edge or Opera on a desktop computer.

- **New installation:** erase the device when prompted.
- **Existing configured device:** leave **Erase device** unchecked. Erasing
  permanently removes saved Wi-Fi, BLE, VPN and MQTT configuration.
- Wait for **Installation complete**, select **Next**, and allow approximately
  30 seconds for the first boot.

Normal updates do not use the USB flasher. Automatic stable updates are enabled
by default and can be disabled in the **System** tab; the same card also allows
a manual update. An administrator can enroll a test device into the clearly
marked **Beta firmware · UNSTABLE** channel with its private OTA key. MeshGate
verifies release origin, board, size and SHA-256 before activating the inactive
OTA slot. NVS and the message mailbox remain intact.

### 2. Configure

1. Join the `Meshtastic-Bridge-Setup` Wi-Fi network.
2. Open `http://192.168.4.1` if the captive portal does not appear.
3. Save Wi-Fi credentials.
4. Scan for the Meshtastic BLE radio, select it and enter its six-digit PIN.
5. Test the BLE connection, save it and restart the bridge.

### 3. Connect a client

| Client | Connection value | Notes |
| --- | --- | --- |
| Meshtastic mobile/desktop app | `<meshgate-ip>:4403` | Add a Network/TCP device |
| Browser-based Meshtastic client | `<meshgate-ip>` | Select HTTP; omit the port; disable HTTPS |
| MeshGate control interface | `http://<meshgate-ip>/` | Use only on LAN or through a private VPN |

Tailscale encrypts the remote path even though the device-local HTTP endpoint
uses HTTP. Never expose TCP `4403`, the Web UI or OTA directly to the public
internet.

## Production target and compatibility

| PlatformIO environment | Hardware | Tailscale | Support level |
| --- | --- | --- | --- |
| `arduino_nano_esp32_idf5` | Arduino Nano ESP32 (ESP32-S3) | Yes | **Only supported production target / public beta** |

MeshGate does not build or publish firmware for ESP32-WROOM, ESP32-S2, generic
ESP32-S3 boards, Seeed XIAO ESP32-S3 or any second recovery profile. The single
Nano production environment uses Arduino Core 3.3.9 and ESP-IDF 5.5.4.

## Reliability and security

MeshGate is designed for unattended recovery, but it does not claim an SLA or
certification. The repository records reproducible, point-in-time measurements
instead of using unqualified reliability claims:

- concurrent HTTP and TCP pressure tests;
- controlled reboot and service-recovery timing;
- queue-drop, mailbox-retention and heap observations;
- release-build and OTA integrity checks;
- known stress boundaries and retest requirements.

See [Validation & Reliability](docs/validation.md) for the current evidence and
[SECURITY.md](SECURITY.md) for private vulnerability reporting and deployment
guidance.

Secrets are write-only through the Web API. Configuration is mirrored between
primary and backup NVS namespaces. Official OTA writes only the inactive
application slot and suppresses scheduled, watchdog and manual restart paths
while the update owns that slot.

## Known limitations

- Public beta means interfaces and operational defaults may still change.
- The strongest runtime coverage is on the Arduino Nano ESP32 production target.
- ESP32 Wi-Fi and BLE share one 2.4 GHz radio; weak Wi-Fi can increase latency.
- Browser clients cannot call an HTTP device from an HTTPS page because of
  browser mixed-content rules.
- The mailbox stores decoded text on the local data partition. Anyone with
  device or private Web API access can read retained messages.
- Tailscale and classic WireGuard are mutually exclusive in the IDF 5 build.

## Documentation

| Document | Purpose |
| --- | --- |
| [User guide](docs/user_guide.md) | Setup, clients, VPN, mailbox, MQTT and monitoring |
| [Architecture](docs/architecture.md) | Runtime design, data paths and embedded constraints |
| [Configuration UI and API](docs/configuration_ui.md) | Web interface, endpoints and persistence |
| [Validation & Reliability](docs/validation.md) | Measured baselines, limits and release gates |
| [Development guide](docs/development.md) | Builds, profiling, testing and releases |
| [Source project comparison](docs/upstream-differences.md) | Differences from SourceLabOrg/meshtastic-esp32-bt-bridge |
| [Technical changelog](CHANGELOG.md) | Firmware changes, newest first |
| [License policy](LICENSE_POLICY.md) | Combined-work licensing and distribution obligations |

## Build from source

```powershell
git clone --recurse-submodules https://github.com/p1337/meshgate.git
cd meshgate
pio run -e arduino_nano_esp32_idf5
```

The production toolchain is pinned in `platformio.ini`. See
[CONTRIBUTING.md](CONTRIBUTING.md) before submitting a change. Pull requests
must pass the repository's Arduino Nano ESP32 production build check.

## Project origin, attribution and license

MeshGate is a source fork of
[SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge).
The repository was created independently rather than through GitHub's Fork
button, so GitHub may not display the usual fork-network banner. The original
MIT copyright, permission notice and authorship remain preserved for derived
portions. MeshGate also builds on the valuable original work and idea of
[Crim](https://github.com/Crim).

The combined MeshGate firmware is distributed under **GPL-3.0-only** because it
generates and links GPLv3 Meshtastic Protocol Buffer code. Binary releases
include corresponding source archives with the exact protobuf revision used by
the build. Commercial use is permitted under GPLv3, subject to its source,
license, notice and modification requirements. See [LICENSE](LICENSE),
[LICENSE_POLICY.md](LICENSE_POLICY.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

MeshGate is an independent community project and is not affiliated with,
sponsored by or endorsed by Meshtastic LLC. Meshtastic® is a registered
trademark of Meshtastic LLC; references here describe compatibility only.

## Hardware

The production build is developed and tested on the
**Arduino Nano ESP32 (ESP32-S3, without headers)**.

[Tested board on Amazon](https://link.amazon/B0bHRPLPo) ·
[Install with the Web Flasher](https://p1337.github.io/meshgate/)

## Contributing and support

- Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.
- Use the structured issue forms for reproducible bugs and focused proposals.
- Read [SUPPORT.md](SUPPORT.md) for support boundaries and diagnostic data.
- Report security issues privately as described in [SECURITY.md](SECURITY.md).

MeshGate is maintained by **Robert Stefanowicz (p1337)**. Contact the maintainer
through [LinkedIn](https://de.linkedin.com/in/robert-stefanowicz-367304180).

<p align="center">
  <a href="https://paypal.me/p1337/3EUR">☕ Support MeshGate development with €3</a>
</p>

<p align="center">
  <a href="https://flagcounter.me/details/kOa">
    <img src="https://flagcounter.me/kOa" alt="MeshGate visitors by country">
  </a>
</p>
