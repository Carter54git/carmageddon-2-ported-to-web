#!/bin/sh
# Headless native build of the runtime (Linux/macOS, for testing and profiling).
#   sh src/native/build.sh && ./emu <path to Carmageddon II folder>
H=$(cd "$(dirname "$0")/.." && pwd)
${CC:-cc} -O2 -g -Wall -Wno-unused-function -I"$H" "$H/native/main.c" "$H/x86cpu.c" "$H/emu_core.c" "$H/emu_main.c" \
  "$H/win32.c" "$H/dx.c" "$H/glide.c" "$H/jit.c" -lm -o "${OUT:-emu}"
