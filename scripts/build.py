#!/usr/bin/env python3
"""Compile all benchmark tools into ./bin with GCC (MinGW-w64 on Windows)."""
import os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC, BIN = ROOT / "src", ROOT / "bin"
EXE = ".exe" if os.name == "nt" else ""
TOOLS = ["lat", "bw", "loaded", "kernel", "sysprobe", "runner"]

# -O3 + -march=native lets GCC vectorise the streaming loops; -fno-tree-loop-distribute-patterns stops it turning
# the write loop into a memset call. Exact flags are recorded in data/build_info.json for the report.
CFLAGS = ["-O3", "-march=native", "-std=gnu11", "-Wall", "-Wextra", "-fno-tree-loop-distribute-patterns"]


def find_cc():
    for cc in (os.environ.get("CC"), "gcc", "x86_64-w64-mingw32-gcc", "clang"):
        if cc and shutil.which(cc):
            return cc
    sys.exit("No gcc/clang found on PATH. On Windows install MSYS2 and run:\n"
             "  pacman -S mingw-w64-ucrt-x86_64-gcc   (then use the 'MSYS2 UCRT64' shell, or add its bin dir to PATH)\n"
             "See docs/WINDOWS_SETUP.md")


def main():
    cc = find_cc()
    ver = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    BIN.mkdir(exist_ok=True)
    libs = ["-ladvapi32"] if os.name == "nt" else ["-lpthread", "-lm"]
    for t in TOOLS:
        out = BIN / (t + EXE)
        cmd = [cc, *CFLAGS, str(SRC / (t + ".c")), "-o", str(out), *libs]
        print(" ".join(cmd))
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr)
            sys.exit(f"build of {t} failed")
        # warnings are shown but not fatal
        if r.stderr.strip():
            print(r.stderr.strip()[:600])
    (ROOT / "data").mkdir(exist_ok=True)
    import json
    (ROOT / "data" / "build_info.json").write_text(json.dumps({"compiler": ver, "cflags": CFLAGS, "libs": libs}, indent=2))
    print(f"\nBuilt {len(TOOLS)} tools with: {ver}\n-> {BIN}")


if __name__ == "__main__":
    main()
