# MeshGate technical changelog

Firmware, runtime, security, compatibility, API, build and release-pipeline
changes are listed newest first. Documentation-only and promotional changes
are intentionally omitted.

## [v0.7.15] - 2026-11-11

- Fixed scheduled 00:00/06:00/12:00/18:00 maintenance restarts using UTC
  instead of the deployment's CET/CEST local time.

## [v0.7.14] - 2026-11-11

- Added memory-aware HTTP admission control with independent reserved capacity
  for status, recovery, OTA and Meshtastic PhoneAPI requests.
- Streamed the retained diagnostic log directly from its bounded ring instead
  of duplicating the complete log in a temporary internal-heap string.
- Made the **Not checked** release value start an explicit metadata-only check
  without downloading or installing firmware.

## [v0.7.13] - 2026-11-11

- Pre-compressed the embedded control page in flash, reducing first-load
  traffic over Tailscale DERP while retaining a raw fallback for older clients.

## [v0.7.12] - 2026-11-11

- Moved the persistent recovery supervisor away from the network core and made
  Web, API and OTA restart requests allocation-free under low-memory conditions.
- Added sustained internal-heap fragmentation recovery while explicitly
  suppressing automatic restarts throughout OTA download and flash validation.

## [v0.7.11] - 2026-11-11

- Performance and reliability polishing.

## [v0.7.10] - 2026-11-10

- Fixed the Tailscale enrollment waiting tab remaining on `about:blank` when
  MeshGate already had an approved persistent identity.
- Made the waiting tab poll enrollment state independently and return to the
  Remote VPN page after an existing or newly approved connection is ready.
- Reduced the metadata-only release-check task stack and made automatic checks
  defer and retry quietly during transient DERP/MQTTS startup memory pressure.
- Added the MeshGate modification copyright attribution to the embedded
  control interface while retaining the original SourceLab.org notice.

## [v0.7.9] - 2026-11-10

- Made the large MeshGate header logo in the embedded control interface link
  to the canonical project repository.

## [v0.7.8] - 2026-11-10

- Replaced the embedded raster logo with a native 1.7 KB SVG composed only of
  the MeshGate symbol and wordmark paths.
- Marked the main control-page response as non-cacheable so a browser always
  receives the asset references belonging to the running firmware after OTA.

## [v0.7.7] - 2026-11-10

- Reduced the embedded logo payload from 346 KB to 75 KB while preserving its
  transparent appearance, preventing large image transfers from stalling HTTP
  diagnostics and competing with Meshtastic traffic over Tailscale DERP.
- Added explicit image dimensions and a firmware-version cache key so mobile
  browsers recover cleanly from an interrupted or previously cached logo load.

## [v0.7.6] - 2026-11-10

- Embedded the canonical MeshGate logo in firmware flash and added the local
  `/assets/meshgate-logo.png` endpoint for the offline control interface.
- Reused the same versioned logo asset in the device browser icon, Web Flasher,
  repository documentation and generated GitHub Release notes.

## [v0.7.5] - 2026-11-10

- Added a checksummed LittleFS mailbox that retains the newest 128 decoded
  Meshtastic text messages across reboots and OTA updates.
- Added automatic mailbox replay through native TCP and HTTP PhoneAPI clients
  after the normal Meshtastic configuration handshake.
- Added cursor-based `GET /api/mailbox` access for retained messages.
- Added an automatic, non-installing official-release check whenever the
  connected control page opens outside setup AP mode.
- Made the installed and latest version numbers link to this technical
  changelog.
- Changed the release workflow so every future firmware tag must have a
  matching changelog entry, which is published as its GitHub Release notes.

## [v0.7.4] - 2026-11-10

- No firmware or runtime changes.

## [v0.7.3] - 2026-11-10

- Split the device control interface into persistent Connection, Remote VPN,
  Services and System tabs without interrupting polling or edit state.

## [v0.7.2] - 2026-11-10

- Corrected the embedded project destination.
- Separated the native Meshtastic app address (`100.x:4403`) from the bare
  address required by the browser-based HTTP client.

## [v0.7.1] - 2026-11-10

- Added one-click Tailscale onboarding with automatic reboot recovery,
  browser-login redirection and approval polling.
- Added offline QR and platform-specific Tailscale installation links.

## [v0.7.0] - 2026-11-10

- Added keyless native Tailscale browser enrollment through MicroLink.
- Added standard Meshtastic HTTP PhoneAPI endpoints for browser clients.
- Added direct and DERP path reporting for remote peers.

## [v0.6.8] - 2026-11-08

- Made the USB recovery and application startup state explicit and observable.
- Ensured the Nano ESP32 RGB LED is released from ROM/WebSerial state during
  production startup.

## [v0.6.7] - 2026-11-08

- Made official OTA resilient under TLS memory pressure by pausing MQTT during
  the update and safely resuming interrupted HTTPS downloads.

## [v0.6.6] - 2026-11-08

- No firmware or runtime changes.

## [v0.6.5] - 2026-11-08

- Republished the v0.6.4 source state; no firmware source change from v0.6.4.

## [v0.6.4] - 2026-11-08

- Added one-click official OTA using fixed GitHub Pages metadata, board and
  size validation, SHA-256 verification and inactive-slot installation.

## [v0.6.3] - 2026-11-08

- Fixed OTA key provisioning by registering the specific token endpoint before
  the parent OTA upload route.

## [v0.6.2] - 2026-11-08

- Added authenticated browser OTA, dual-slot validation and remote update
  diagnostics.

## [v0.6.1] - 2026-11-08

- Fixed production release builds by installing protobuf tooling in the
  PIOArduino environment used by the IDF-5 toolchain.

## [v0.6.0] - 2026-11-08

- Added the Arduino Nano ESP32 production profile on Arduino Core 3.3.9 and
  ESP-IDF 5.5.4.
- Added native Tailscale remote access with MicroLink, DERP relay transport and
  corrected lwIP packet routing.
- Added redundant configuration, watchdog recovery, authenticated OTA, remote
  diagnostics and the ESP32-S3 release/Web-Flasher pipeline.

## [v0.5.0] - 2026-10-23

- Updated the bundled public CA certificate data used for TLS validation.

## [v0.4.0] - 2026-10-13

- Added Prometheus/OpenMetrics telemetry and consolidated diagnostic metrics.

## [v0.3.0] - 2026-10-09

- Added the Meshtastic MQTT proxy gateway and radio configuration sync.
- Added the unified `/status` endpoint, queue/drop telemetry and non-stacking
  client polling.
- Moved bridge queues to fixed static allocation and tightened packet bounds.

## [v0.2.0] - 2026-10-07

- Refactored runtime logging, compile-time log levels and the debug toggle.
- Cleaned up the control interface, build flags and Web Flasher generation.

## [v0.1.0] - 2026-10-06

- Initial ESP32 BLE-to-TCP Meshtastic bridge release.

[v0.7.15]: https://github.com/p1337/meshgate/compare/v0.7.14...v0.7.15
[v0.7.14]: https://github.com/p1337/meshgate/compare/v0.7.13...v0.7.14
[v0.7.13]: https://github.com/p1337/meshgate/compare/v0.7.12...v0.7.13
[v0.7.12]: https://github.com/p1337/meshgate/compare/v0.7.11...v0.7.12
[v0.7.11]: https://github.com/p1337/meshgate/compare/v0.7.10...v0.7.11
[v0.7.10]: https://github.com/p1337/meshgate/compare/v0.7.9...v0.7.10
[v0.7.9]: https://github.com/p1337/meshgate/compare/v0.7.8...v0.7.9
[v0.7.8]: https://github.com/p1337/meshgate/compare/v0.7.7...v0.7.8
[v0.7.7]: https://github.com/p1337/meshgate/compare/v0.7.6...v0.7.7
[v0.7.6]: https://github.com/p1337/meshgate/compare/v0.7.5...v0.7.6
[v0.7.5]: https://github.com/p1337/meshgate/compare/v0.7.4...v0.7.5
[v0.7.4]: https://github.com/p1337/meshgate/compare/v0.7.3...v0.7.4
[v0.7.3]: https://github.com/p1337/meshgate/compare/v0.7.2...v0.7.3
[v0.7.2]: https://github.com/p1337/meshgate/compare/v0.7.1...v0.7.2
[v0.7.1]: https://github.com/p1337/meshgate/compare/v0.7.0...v0.7.1
[v0.7.0]: https://github.com/p1337/meshgate/compare/v0.6.8...v0.7.0
[v0.6.8]: https://github.com/p1337/meshgate/compare/v0.6.7...v0.6.8
[v0.6.7]: https://github.com/p1337/meshgate/compare/v0.6.6...v0.6.7
[v0.6.6]: https://github.com/p1337/meshgate/compare/v0.6.5...v0.6.6
[v0.6.5]: https://github.com/p1337/meshgate/compare/v0.6.4...v0.6.5
[v0.6.4]: https://github.com/p1337/meshgate/compare/v0.6.3...v0.6.4
[v0.6.3]: https://github.com/p1337/meshgate/compare/v0.6.2...v0.6.3
[v0.6.2]: https://github.com/p1337/meshgate/compare/v0.6.1...v0.6.2
[v0.6.1]: https://github.com/p1337/meshgate/compare/v0.6.0...v0.6.1
[v0.6.0]: https://github.com/p1337/meshgate/compare/v0.5.0...v0.6.0
[v0.5.0]: https://github.com/p1337/meshgate/compare/v0.4.0...v0.5.0
[v0.4.0]: https://github.com/p1337/meshgate/compare/v0.3.0...v0.4.0
[v0.3.0]: https://github.com/p1337/meshgate/compare/v0.2.0...v0.3.0
[v0.2.0]: https://github.com/p1337/meshgate/compare/v0.1.0...v0.2.0
[v0.1.0]: https://github.com/p1337/meshgate/releases/tag/v0.1.0
