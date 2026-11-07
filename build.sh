#!/usr/bin/env sh
# Cross-compile build/AutoClicker.exe from Linux/macOS. Needs Zig (or: pip install ziglang).
set -e
cd "$(dirname "$0")"
mkdir -p build
if command -v zig >/dev/null 2>&1; then ZIG="zig"; else ZIG="python3 -m ziglang"; fi
$ZIG cc -target x86_64-windows-gnu -O2 -s -municode -Wl,--subsystem,windows \
  src/autoclicker.c res/app.rc -o build/AutoClicker.exe \
  -luser32 -lgdi32 -lcomctl32 -lwinmm -lmsimg32
rm -f build/*.pdb
echo "Built build/AutoClicker.exe"
