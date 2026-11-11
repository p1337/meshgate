# Fork and Source Project Differences

MeshGate is a source fork of
[SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge).
The repository was imported into a new GitHub repository rather than created
with GitHub's Fork button. This document provides the provenance and comparison
that the GitHub network banner therefore cannot show.

## Foundation retained from [SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge)

- ESP32 BLE client for the Meshtastic GATT service
- transparent Meshtastic PhoneAPI transport on TCP port 4403
- captive-portal configuration flow
- Wi-Fi and mDNS integration
- MQTT proxy architecture
- MIT-licensed foundation and original authorship

## License and attribution

The portions derived from
[SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge)
retain the original SourceLab.org MIT copyright and permission notice in
`LICENSES/SourceLabOrg-MIT.txt`. MeshGate-specific work is copyrighted by
Robert Stefanowicz. The combined firmware generates and links code from the
GPLv3 Meshtastic Protocol Buffers and is therefore conveyed under
`GPL-3.0-only`. The root `LICENSE` contains GPLv3; component notices and exact
provenance are collected in `THIRD_PARTY_NOTICES.md`. Attribution does not
imply endorsement of a third-party product.

## MeshGate additions

| Area | [SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge) | MeshGate |
| --- | --- | --- |
| Remote access | Local LAN | Native Tailscale/MicroLink and classic WireGuard |
| Runtime generation | Arduino Core 2 / ESP-IDF 4 | Additional production target on Arduino Core 3.3.9 / ESP-IDF 5.5.4 |
| BLE stack | NimBLE-Arduino 1.x | NimBLE-Arduino 2.5.2 in the IDF 5 target |
| Tailscale data plane | Not present | Coordination, keyless browser enrollment, DERP, WireGuard, peer status and TCP/HTTP access |
| lwIP delivery | Not applicable | Decrypted packets are queued onto the lwIP TCPIP thread |
| Mobile relay routing | Not applicable | DERP remains authoritative until DISCO proves a direct endpoint |
| Concurrent TLS | One broker path | Remote-first startup plus PSRAM-backed large mbedTLS buffers for DERP and MQTTS |
| OTA | Basic firmware lifecycle | Authenticated dual-slot OTA with MD5 and post-reboot verification |
| Reliability | Normal reconnect behavior | Redundant NVS, task watchdog, scheduled restart and outage recovery |
| Diagnostics | Serial / UI status | Remote log ring, JSON status, OpenMetrics and controlled reboot API |
| BLE recovery | Saved address | Service/name rediscovery for rotating nRF52 private addresses |
| Secret handling | Configuration storage | Browser-based Tailscale enrollment plus write-only BLE PIN, VPN keys and OTA token APIs |
| Hardware | Existing ESP32 targets | Arduino Nano ESP32 recovery and IDF 5 production profiles |
| Interface | Light card UI | Self-contained MeshRadar-inspired dark operations UI |

## “Version 5” meaning

Version 5 refers to the ESP-IDF generation, not Meshtastic firmware 2.5 and not
a MeshGate release number. The production environment is
`arduino_nano_esp32_idf5` and currently resolves to:

- Arduino Core 3.3.9
- ESP-IDF 5.5.4
- NimBLE-Arduino 2.5.2
- MicroLink for Tailscale-compatible networking

The `arduino_nano_esp32` ESP-IDF 4 build remains intentionally available as a
USB recovery image.

## Platform scope

The repository builds five firmware environments:

1. `arduino_nano_esp32_idf5` — production Nano ESP32 with Tailscale
2. `arduino_nano_esp32` — Nano ESP32 recovery image
3. `seeed_xiao_esp32s3` — compact ESP32-S3 bridge
4. `esp32-s3-devkitc-1` — generic ESP32-S3 bridge
5. `esp32dev` — classic ESP32-WROOM / NodeMCU bridge

Only the Nano IDF 5 target includes native Tailscale today. The shared bridge,
MQTT, Web UI, diagnostics, persistence and OTA code remains common wherever
the platform supports it.

## Keeping the fork reviewable

The `upstream` Git remote should point to
[SourceLabOrg/meshtastic-esp32-bt-bridge](https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge).
New work from that original project is reviewed and merged deliberately; the
IDF 5 networking changes must not be replaced by an unreviewed bulk merge
because they include required lwIP thread-affinity and DERP-path correctness
fixes.
