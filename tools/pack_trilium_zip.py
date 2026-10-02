#!/usr/bin/env python3
"""Package the research notes into a ZIP for Trilium's Markdown file import.

Trilium's supported import path is a ZIP archive containing a tree of
Markdown files (see the Trilium user guide, Import & Export > Markdown).
It derives each note's title from the *filename*, so the notes are staged
under their H1 titles rather than their repository filenames. Leaving them
as `FINDINGS.md` would import a note titled "FINDINGS" whose body opens
with an unrelated heading.

    python3 tools/pack_trilium_zip.py [-o dist/msm8953-notes.zip]
"""

from __future__ import annotations

import argparse
import re
import shutil
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCES = [
    "FINDINGS.md",
    "attack_surface_map.md",
    "trustzone_vulnerability_inventory.md",
    "flash_delivery_paths.md",
]
H1_RE = re.compile(r"^#\s+(.*?)\s*$", re.M)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default="dist/msm8953-notes.zip")
    args = ap.parse_args()

    out = (REPO / args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "msm8953-trustzone"
        stage.mkdir()
        for src in SOURCES:
            text = (REPO / src).read_text()
            m = H1_RE.search(text)
            if not m:
                raise SystemExit(f"{src}: no H1 title found")
            title = m.group(1)
            (stage / f"{title}.md").write_text(text)
            print(f"  {src}  ->  {title}.md")

        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            for f in sorted(stage.iterdir()):
                z.write(f, f"msm8953-trustzone/{f.name}")

    print(f"\nwrote {out}  ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
