#!/bin/sh
# Build the browser runtime and assemble a ready-to-host site in dist/site.
#   ./build.sh                 (emcc must be in PATH, or set EMCC=/path/to/emcc)
# Game data is NOT included: run tools/make_data.py afterwards (see README.md).
set -e
cd "$(dirname "$0")"
OUT=dist/site
mkdir -p "$OUT/music"
OUT="$OUT" EMCC="${EMCC:-emcc}" sh src/web/build.sh
cp src/web/index.html src/web/config.js "$OUT/"
cp src/web/htaccess "$OUT/.htaccess"
cp docs/music-README.txt "$OUT/music/README.txt"
rm -rf dist/server && cp -r server dist/server
echo "Built $OUT (carma2hy.js, carma2hy.wasm). Now add the game data:"
echo "  python3 tools/make_data.py <path to Carmageddon II> $OUT"
