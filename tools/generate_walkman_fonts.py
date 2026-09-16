#!/usr/bin/env python3
"""Generate/check Walkman subsets of LXGW WenKai Screen v1.522 (SIL OFL)."""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
parser.add_argument("--font", type=Path, help="Original LXGWWenKaiScreen.ttf v1.522; see assets/README.md")
args = parser.parse_args()
if not args.check:
    if not args.font:
        parser.error("--font is required for generation; see assets/README.md")
    if hashlib.sha256(args.font.read_bytes()).hexdigest() != "cd1a6fa39c4ea42fd8f4e289945789b0e510cf7016435640f8893cdad9b220f3":
        parser.error("font does not match the pinned v1.522 release")
source = "".join((ROOT / path).read_text() for path in ["main/walkman_app.c", "main/walkman_tracks.c", "main/online_setup.c", "main/walkman_online.c"])
symbols = "".join(sorted({ch for ch in source if ord(ch) > 127}))
reply_punctuation = {0x00AB, 0x00BB, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026}
for size in (12, 14, 16):
    name = f"walkman_{size}"
    target = ROOT / f"assets/fonts/{name}.c"
    if args.check:
        present = {int(n, 16) for n in re.findall(r"U\+([0-9A-Fa-f]+)", target.read_text())}
        missing = {ord(c) for c in symbols} - present
        if size == 14:
            missing |= reply_punctuation - present
        if missing:
            raise SystemExit("Missing glyphs: " + "".join(chr(n) for n in sorted(missing)))
        print(f"Walkman {size}px: {len(symbols)} glyphs covered")
        continue
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "npx", "--yes", "lv_font_conv@1.5.3", "--size", str(size), "--bpp", "4",
        "--format", "lvgl", "--font", str(args.font),
        "--symbols", symbols, "--range", "0x20-0x7e,0xab,0xbb,0x2014,0x2018-0x2019,0x201c-0x201d,0x2026,0x4e00-0x9fff,0x3000-0x303f,0xff00-0xffef" if size==14 else "0x20-0x7e", "--no-compress", "--no-kerning",
        "--lv-include", "lvgl.h", "--lv-font-name", name, "-o", str(target),
    ], check=True)
    content = target.read_text().replace(str(args.font), "LXGWWenKaiScreen.ttf").replace(str(ROOT), "<repo>")
    target.write_text("/* Walkman bitmap subset; source: LXGW WenKai Screen v1.522.\n"
                      " * SIL OFL 1.1: see OFL-WenKai.txt. */\n" + content.rstrip() + "\n")
