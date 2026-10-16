#!/usr/bin/env python3
"""Empacota libRenderScaleBedrock.so em um .levipack (zip com manifest.json)."""
import argparse
import json
import zipfile
from pathlib import Path

MANIFEST = {
    "type": "preload-native",
    "name": "RenderScaleBedrock",
    "author": "RenderScale Bedrock port",
    "description": "Escala so a resolucao do 3D, mantendo o HUD nitido.",
    "version": "0.3.0-redirect",
    "entry": "libRenderScaleBedrock.so",
    "icon": "",
    "minecraft_versions": ["1.26.5*"],
    "overwrite_files": [],
    "overwrite_folders": [],
}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    if not args.library.is_file():
        raise FileNotFoundError(args.library)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        args.output.unlink()
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(MANIFEST, indent=2) + "\n")
        z.write(args.library, "libRenderScaleBedrock.so")
    with zipfile.ZipFile(args.output) as z:
        names = z.namelist()
        assert "manifest.json" in names and "libRenderScaleBedrock.so" in names, names
    print(f"OK: {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
