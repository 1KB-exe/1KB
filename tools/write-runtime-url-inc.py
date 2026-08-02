#!/usr/bin/env python3
"""Generate the MASM URL definition from Config.h's authoritative constant."""
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r'LauncherRuntimeDownloadUrl\[\]\s*=\s*L"([^"\r\n]+)"', source)
url = match.group(1) if match else ""
if not url.startswith("https://") or not url.isascii() or "'" in url:
    raise SystemExit("invalid LauncherRuntimeDownloadUrl in Config.h")
wide = ",".join(f"'{character}'" for character in url)
Path(sys.argv[2]).write_text(
    f"ONEKB_RUNTIME_URL_WIDE TEXTEQU <{wide}>\n",
    encoding="ascii",
)
