# Validation & Reliability

MeshGate is a public-beta embedded gateway, not a certified appliance. This
document records reproducible observations from the production hardware and
defines the checks expected before a release. Results are point-in-time
baselines, not an uptime guarantee or service-level agreement.

## Validated production configuration

| Component | Validated configuration |
| --- | --- |
| Gateway | Arduino Nano ESP32 (ESP32-S3) |
| Build | `arduino_nano_esp32_idf5` |
| Runtime | Arduino Core 3.3.9 / ESP-IDF 5.5.4 |
| BLE | NimBLE-Arduino 2.5.2 |
| Remote access | Native Tailscale through MicroLink, including DERP |
| Radio transport | Meshtastic BLE GATT to TCP `4403` and HTTP PhoneAPI |
| Persistent state | Mirrored NVS configuration and LittleFS mailbox |

No other PlatformIO environments are supported, compiled by CI or published.

## Recorded baselines

The following observations were collected on the production Nano profile on
2026-11-12. Full implementation notes and the exact constraints that must be
preserved are maintained in [development.md](development.md).

| Scenario | Observed result | Release significance |
| --- | --- | --- |
| Mixed HTTP pressure | Eight rounds across `/status`, `/metrics`, `/api/logs`, mailbox and SVG paths completed while BLE and MQTTS stayed connected | Reserved recovery paths and low-memory admission control must remain available |
| Fragmented-heap release check | Metadata-only official release checks use a PSRAM-backed 8 KiB stack; the flash-writing installer retains an internal stack | Release discovery must remain available when the largest internal block is below 8 KiB without moving flash-sensitive execution into PSRAM |
| Sequential TCP churn | 20 sequential TCP `4403` connections succeeded | Basic connection ownership and cleanup path exercised |
| Full TCP capacity | Three simultaneous TCP clients connected without bridge-queue drops | Confirms the configured production client limit |
| Full slots plus HTTP | With all three TCP slots held, 18 concurrent-path requests returned `200`; slowest response was 6.5 s on a weak `-90 dBm` Wi-Fi link | Correctness confirmed at the stress boundary; latency is not a throughput target |
| Controlled reboot | `POST /api/reboot` returned `202`; LAN HTTP and TCP returned after 15.8 s; BLE, MQTTS, Tailscale and DERP were ready after 20.2 s | Recovery must preserve NVS, mailbox contents and zero queue-drop state |
| Mailbox persistence | Retained records and sequence cursor survived the controlled reboot | OTA and ordinary reboot must not erase the data partition |
| Remote data plane | An iPhone on mobile data reached the control page and TCP `4403` through Tailscale DERP | Proves traffic flow, not only control-plane registration |

### Heap and load-shedding boundary

Early HTTP pressure tests drove the historical minimum internal heap to 476 B
and later 184 B before recovery. The production policy was therefore tightened
to require either 32 KiB free internal heap or a 16 KiB largest internal block
for ordinary work. In low-memory mode, MeshGate admits two ordinary responses
at a time and preserves two recovery slots. A bounded `503` with `Retry-After`
under this condition is intentional load shedding, not proof that the recovery
plane failed.

Dynamic responses use `BoundedStreamResponse` with acknowledged 512-byte
blocks. Replacing it with `AsyncResponseStream` or `beginResponseStream()` can
create a send-window-sized allocation and is not acceptable on the production
path without repeating the full pressure test.

### TCP ownership regression

A later 30-connection churn run found that accepted `AsyncClient` instances
were not released after their per-client context was destroyed. The current
implementation releases both objects and applies the same deferred cleanup to
the max-client rejection path. The Arduino Nano ESP32 production profile
compiles with the fix.

Before this item is considered closed for a stable release, repeat the
30-connection test on production hardware and confirm that free heap returns to
its pre-test range after lwIP cleanup. A zero active-client count alone is not
sufficient.

## Continuous verification

The repository currently provides:

- a pull-request workflow that builds the single supported Arduino Nano ESP32
  production environment;
- a tag-driven production release build;
- a version-to-tag consistency check;
- SHA-256 checksums for all release assets;
- a complete corresponding-source archive containing the exact protobuf
  submodule revision;
- GitHub Pages metadata for board, size, URL and SHA-256 verified OTA.

CI compilation is necessary but does not replace hardware validation.

## Release acceptance checklist

Complete the applicable checks before promoting a public-beta build:

- [ ] The `arduino_nano_esp32_idf5` production environment compiles in CI.
- [ ] Production firmware boots on Arduino Nano ESP32 without a reset loop.
- [ ] Setup AP, captive portal and configuration persistence work.
- [ ] A cold control-page load completes configuration, `/status`, both VPN
      status reads, OTA status and mailbox initialization without leaving
      permanent `Loading` or `Unavailable` values under low-memory admission.
- [ ] BLE pairing, reconnect and bidirectional PhoneAPI traffic work.
- [ ] TCP `4403` and HTTP PhoneAPI work with supported clients.
- [ ] Tailscale shows both control-plane registration and a usable data path.
- [ ] MQTT TLS and BLE remain connected during ordinary Web UI use.
- [ ] Official OTA validates metadata, image size and SHA-256, switches slots
      and retains NVS plus mailbox data.
- [ ] Manual reboot returns `202` and restores critical services.
- [ ] Queue-drop counters remain zero during the standard pressure scenario.
- [ ] TCP churn returns heap to its pre-test range after connection cleanup.
- [ ] No secret, local OTA token or deployment credential is tracked.
- [ ] Release notes describe technical behavior without unverified claims.

## Reporting a regression

Use the repository's bug-report form and include firmware version, board,
transport path, reproduction steps, `/status` output with secrets removed and
the relevant bounded `/api/logs` excerpt. Security-sensitive reports belong in
the private process described by [SECURITY.md](../SECURITY.md).
