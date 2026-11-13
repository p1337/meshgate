# Security Policy

## Supported versions

Security fixes are provided for the latest published MeshGate release. Before
reporting a problem, confirm whether it is reproducible on that version. Older
public-beta builds may be asked to update before investigation.

## Reporting a vulnerability

Please report suspected vulnerabilities privately through
[GitHub Security Advisories](https://github.com/p1337/meshgate/security/advisories/new).
Do not disclose the issue publicly until a fix or mitigation is available.

Include, where possible:

- affected firmware version and hardware;
- attack preconditions and network position;
- concise reproduction steps or proof of concept;
- expected and observed impact;
- relevant logs with credentials, keys, addresses and private messages removed;
- any suggested mitigation.

The maintainer will acknowledge the report, validate its impact and coordinate
disclosure based on severity and fix readiness. This project does not promise a
fixed response-time SLA.

## Deployment boundaries

- Do not expose TCP `4403`, the control interface or OTA directly to the public
  internet. Use an authenticated private network such as Tailscale or
  WireGuard.
- Treat access to the control interface as administrative access.
- Treat physical access to the ESP32 data partition as access to retained
  mailbox text.
- Keep device, tailnet and broker credentials unique and revoke them after a
  suspected compromise.
- Use the official release channel and verify published checksums when
  installing files manually.
- Never enable certificate-validation bypass for an internet broker.

## Secret handling

Wi-Fi passwords, BLE PINs, VPN private keys, legacy Tailscale auth keys and OTA
tokens are write-only through the Web API. They must never appear in issues,
logs, screenshots or commits. The local `.ota-token` file is intentionally
ignored by Git.

## Update safety

Official OTA accepts no arbitrary URL or browser-supplied firmware. MeshGate
validates the fixed release metadata, production board, byte length and SHA-256
digest before activating the inactive slot. Restart paths are suppressed while
OTA owns the update slot; an interrupted, unverified image must not replace the
currently bootable application.
