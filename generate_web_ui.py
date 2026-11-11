Import("env")

import gzip
import pathlib
import re


project_dir = pathlib.Path(env.subst("$PROJECT_DIR"))
source_path = project_dir / "src" / "config_ui.cpp"
options_path = project_dir / "include" / "build_options.h"
output_path = project_dir / "web" / "generated" / "index_html.gz"


def read_define(source: str, name: str) -> str:
    match = re.search(rf'^#define\s+{re.escape(name)}\s+"([^"]+)"', source, re.MULTILINE)
    if not match:
        raise RuntimeError(f"Could not resolve {name} for the embedded Web UI")
    return match.group(1)


cpp = source_path.read_text(encoding="utf-8")
options = options_path.read_text(encoding="utf-8")
match = re.search(
    r'const char index_html\[\] PROGMEM = R"rawliteral\((.*?)\)rawliteral";',
    cpp,
    re.DOTALL,
)
if not match:
    raise RuntimeError("Could not extract index_html from src/config_ui.cpp")

html = match.group(1)
substitutions = {
    ')rawliteral" FIRMWARE_VERSION R"rawliteral(': read_define(options, "FIRMWARE_VERSION"),
    ')rawliteral" PROJECT_GITHUB_URL R"rawliteral(': read_define(options, "PROJECT_GITHUB_URL"),
}
for marker, value in substitutions.items():
    html = html.replace(marker, value)

if ")rawliteral\"" in html:
    raise RuntimeError("An unresolved C++ raw-string boundary remains in the Web UI")

compressed = gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)
output_path.parent.mkdir(parents=True, exist_ok=True)
if not output_path.exists() or output_path.read_bytes() != compressed:
    output_path.write_bytes(compressed)

print(f"[generate_web_ui] {len(html.encode('utf-8'))} -> {len(compressed)} bytes")
