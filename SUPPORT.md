# Support

MeshGate is a community-maintained public-beta project. Support is best effort.

## Where to ask

- **Reproducible defect:** use the bug-report issue form.
- **Focused improvement:** use the feature-request issue form.
- **Security concern:** use the private process in [SECURITY.md](SECURITY.md).
- **General setup:** first consult the [User guide](docs/user_guide.md) and
  existing issues.

## Information that helps

For a technical issue, include:

- MeshGate version and PlatformIO target;
- exact ESP32 board and Meshtastic radio model;
- local LAN, Tailscale, WireGuard or MQTT path involved;
- clear reproduction steps;
- expected and observed result;
- sanitized `/status` output and a short `/api/logs` excerpt;
- whether the issue survives a controlled reboot.

Remove Wi-Fi credentials, BLE PINs, VPN keys, OTA tokens, private IP details
that are not required, and private message contents. Do not post screenshots
that reveal secrets.

## Scope

The production target receives priority. Community build targets are welcome,
but hardware-specific fixes may require access to that board and evidence from
the reporter. Questions about Meshtastic radio behavior that also occurs
without MeshGate should be directed to the Meshtastic project.
