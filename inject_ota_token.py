from pathlib import Path
import re

Import("env")

token_file = Path(env["PROJECT_DIR"]) / ".ota-token"
if token_file.exists():
    token = token_file.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", token):
        raise RuntimeError(".ota-token must contain exactly 64 hexadecimal characters")
    env.Append(CPPDEFINES=[("OTA_BOOTSTRAP_TOKEN", '\\"%s\\"' % token.lower())])
else:
    print("[ota] .ota-token absent; authenticated OTA remains disabled for this build")
