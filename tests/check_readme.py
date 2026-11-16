#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 saalineo
"""Fails unless README.md embeds examples/readme_malloc.c and examples/readme_mstr.c verbatim."""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parent.parent
readme = (root / "README.md").read_text()
bad = 0
for name in ("readme_malloc.c", "readme_mstr.c"):
    src = (root / "examples" / name).read_text().rstrip("\n")
    if src not in readme:
        print(f"README.md does not contain examples/{name} verbatim"); bad = 1
sys.exit(bad)
