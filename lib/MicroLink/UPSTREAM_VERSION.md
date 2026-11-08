# Vendored MicroLink version

- Upstream: `https://github.com/CamM2325/microlink`
- Commit: `216da3300f0493b0860247d43f7af5ce29df63a5`
- License: MIT (see `LICENSE`)

The enhanced `wireguard_lwip` sources bundled by MicroLink are used by both
MicroLink/Tailscale and MeshGate's classic WireGuard client. This intentionally
avoids linking the incompatible duplicate implementation from
`WireGuard-ESP32` into the ESP-IDF 5 firmware.
