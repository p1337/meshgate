# Third-party notices

MeshGate combines original MeshGate work with third-party components. The
combined firmware is distributed under **GNU GPL v3 only** (`GPL-3.0-only`).
Individual third-party components retain their original copyright notices and
licenses; the GPL does not erase those notices.

## SourceLabOrg/meshtastic-esp32-bt-bridge

- Source: https://github.com/SourceLabOrg/meshtastic-esp32-bt-bridge
- License: MIT
- Copyright: Copyright (c) 2026 SourceLab.org
- Preserved license: [LICENSES/SourceLabOrg-MIT.txt](LICENSES/SourceLabOrg-MIT.txt)

MeshGate is a source fork of this project. Its original MIT permission and
copyright notice remain applicable to the portions derived from it.

## Meshtastic Protocol Buffers

- Source: https://github.com/meshtastic/protobufs
- Vendored as Git submodule: `proto/meshtastic`
- License: GNU General Public License version 3
- Exact source revision: recorded by the repository's Git submodule pointer
- License copy: `proto/meshtastic/LICENSE`

The build generates Nanopb sources from these protocol definitions and links
the generated code into the firmware. This GPL-covered dependency is the
reason the combined MeshGate firmware is conveyed under `GPL-3.0-only`.

## MicroLink

- Source: https://github.com/kaieda/microlink
- Vendored in: `lib/MicroLink`
- License: MIT
- Preserved license: [LICENSES/MicroLink-MIT.txt](LICENSES/MicroLink-MIT.txt)

## Meshtastic trademarks

Meshtastic® is a registered trademark of Meshtastic LLC. MeshGate is an
independent community project and is not affiliated with, sponsored by, or
endorsed by Meshtastic LLC. References to Meshtastic identify compatibility
and the protocol used by the attached radio; they do not claim ownership of
the trademark.

The official Meshtastic logo is not included in MeshGate branding or the
architecture graphic. Any future use of Meshtastic community project marks
must follow the then-current rules published at
https://meshtastic.org/docs/legal/licensing-and-trademark/.

## No warranty

MeshGate and its included components are provided without warranty to the
extent permitted by their respective licenses. See the root [LICENSE](LICENSE)
and the component license files above for the controlling terms.
