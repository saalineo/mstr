#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 saalineo
"""Builds and runs every complete program in docs/GUIDE.md and the README quick start.

usage: check_guide.py <c-compiler> <cxx-compiler> <repo-root>
A block counts as a program if it starts with #include. Blocks marked `abort!` are the
checked-mode demo: they must build but are not run. Programs run with stdin closed.
"""
import os, pathlib, re, subprocess, sys, tempfile

cc, cxx, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
inc, src = root / "include", root / "src" / "mstr.c"
progs = []
for doc in ("docs/GUIDE.md", "README.md"):
    text = (root / doc).read_text()
    for lang, code in re.findall(r"```(c|cpp)\n(.*?)```", text, re.S):
        if code.lstrip().startswith("#include"):
            progs.append((doc, lang, code))

# the README's two comparison programs are covered by their own tests
progs = [p for p in progs if "normalise" not in p[2] and "readme" not in p[2] and 'MSTR_PIPE(mstr_from(argc' not in p[2]]
bad, env = 0, dict(os.environ, MSTR_NO_ROOT_WARNING="1")
with tempfile.TemporaryDirectory() as tmp:
    obj = pathlib.Path(tmp) / "mstr.o"
    subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(inc), "-c", str(src), "-o", str(obj)], check=True)
    for i, (doc, lang, code) in enumerate(progs):
        f = pathlib.Path(tmp) / f"p{i}.{lang}"
        f.write_text(code)
        exe = pathlib.Path(tmp) / f"p{i}"
        if lang == "c":
            cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(inc), str(f), str(obj), "-o", str(exe), "-pthread"]
        else:
            cmd = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(inc), str(f), str(obj), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        name = f"{doc} program #{i} ({code.strip().splitlines()[1][:40]!r}...)"
        if r.returncode:
            print("BUILD FAILED:", name, r.stderr[:600]); bad = 1; continue
        if "abort!" in code:
            continue
        r = subprocess.run([str(exe)], stdin=subprocess.DEVNULL, capture_output=True, text=True, env=env)
        if r.returncode:
            print("RUN FAILED:", name, "exit", r.returncode, r.stderr[:300]); bad = 1
print(f"{len(progs)} documentation programs checked, {'FAILURES' if bad else 'all ok'}")
sys.exit(bad)
