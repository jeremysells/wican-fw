#!/usr/bin/env python3
"""Embed the upstream UI plus the PAN extension without changing its checkout."""
import gzip
from pathlib import Path
import sys

ui_root, destination = map(Path, sys.argv[1:])
sys.path.insert(0, str(ui_root))
sys.path.insert(0, str(ui_root / "tools"))
from gzip_asset import minify_html

source = (ui_root / "web/index.html").read_text(encoding="utf-8")
anchor = "\nboot();\n"
if source.count(anchor) != 1:
    raise SystemExit("Upstream UI no longer has a unique pre-boot PAN injection point")
extension = Path(__file__).parents[1] / "web/usb-pan.js"
source = source.replace(anchor, "\n" + extension.read_text(encoding="utf-8") + anchor)
destination.write_bytes(gzip.compress(minify_html(source).encode("utf-8"), mtime=0))
