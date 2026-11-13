# Contributing to MeshGate

Thank you for improving MeshGate. The project values focused changes,
reproducible evidence and compatibility with constrained embedded hardware.

## Before starting

1. Search existing issues and pull requests.
2. For a significant behavior or architecture change, open a focused proposal
   before implementation.
3. Read [docs/architecture.md](docs/architecture.md),
   [docs/development.md](docs/development.md) and
   [docs/validation.md](docs/validation.md).
4. Do not add a parallel subsystem until the existing implementation and APIs
   have been evaluated.

## Development setup

```powershell
git clone --recurse-submodules https://github.com/p1337/meshgate.git
cd meshgate
pio run -e arduino_nano_esp32_idf5
```

Run the only supported production target before submitting firmware changes:

```powershell
pio run -e arduino_nano_esp32_idf5
```

The pull-request workflow repeats this single Arduino Nano ESP32 build on a
clean runner. No other board profiles are built or published.

## Pull-request expectations

- Keep the change focused and explain the problem it solves.
- Describe how it was tested, including hardware and network path where
  relevant.
- Preserve bounded allocations in BLE, TCP, HTTP, TLS and mailbox paths.
- Preserve NVS compatibility and inactive-slot OTA safety.
- Add or update documentation when behavior, configuration, APIs or operational
  limits change.
- Add a technical `CHANGELOG.md` entry only for firmware, runtime, security,
  compatibility, API, build or release-pipeline changes. Documentation-only
  changes do not require a firmware release entry.
- Do not commit credentials, tokens, private keys, local deployment files or
  personal network details.

For pressure-sensitive changes, use the scenarios and acceptance criteria in
[docs/validation.md](docs/validation.md). A successful build alone is not
enough for changes to HTTP admission, task stacks, TLS allocation, TCP client
ownership or OTA.

## Commit and review quality

- Use concise, imperative commit subjects.
- Keep generated artifacts out of commits unless the build requires them.
- Avoid unrelated formatting changes.
- Respond to review with new evidence or a targeted revision.
- Mark untested hardware behavior explicitly.

## Licensing

By contributing, you agree that your contribution is distributed as part of
the combined work under `GPL-3.0-only`. Preserve existing copyright, license and
third-party notices. Do not copy code or assets unless their provenance and
license are compatible and documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Security reports

Do not open a public issue for a suspected vulnerability, leaked credential or
unsafe update path. Follow [SECURITY.md](SECURITY.md) instead.
