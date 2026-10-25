#!/bin/sh
# Browser build of the hybrid runtime: carma2hy.js / carma2hy.wasm
# usage: OUT=dir build.sh   (needs emcc in PATH or EMCC=...)
H=${HYBRID:-$(cd "$(dirname "$0")/.." && pwd)}
OUT=${OUT:-.}
EMCC=${EMCC:-emcc}
$EMCC -O3 -I$H $H/web/main.c $H/x86cpu.c $H/emu_core.c $H/emu_main.c $H/win32.c $H/dx.c $H/glide.c $H/jit.c \
  -sGLOBAL_BASE=8388608 -sINITIAL_MEMORY=268435456 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 \
  -sSTACK_SIZE=4194304 -sFORCE_FILESYSTEM=1 -sEXIT_RUNTIME=0 -sENVIRONMENT=web \
  -sEXPORTED_RUNTIME_METHODS=FS,callMain -sEXPORTED_FUNCTIONS=_main,_malloc,_free -sMODULARIZE=0 \
  --pre-js $H/web/pre.js -lidbfs.js --js-library $H/web/library_glide.js --js-library $H/web/library_jit.js -sALLOW_TABLE_GROWTH=1 -o $OUT/carma2hy.js
