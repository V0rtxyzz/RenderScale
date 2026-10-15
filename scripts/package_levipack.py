#!/usr/bin/env python3
"""Empacota libRenderScale.so em um .levipack (zip com manifest.json)."""
import argparse
import json
import zipfile
from pathlib import Path

MANIFEST = {
    "type": "preload-native",
    "name": "RenderScale",
    "author": "RenderScale Bedrock port",
    "description": "Escala a resolucao de renderizacao do 3D sem afetar a UI (usa o upscaling TAAU do jogo).",
    "version": "0.1.0",
    "entry": "libRenderScale.so",
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
        z.write(args.library, "libRenderScale.so")
    with zipfile.ZipFile(args.output) as z:
        names = z.namelist()
        assert "manifest.json" in names and "libRenderScale.so" in names, names
    print(f"OK: {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
