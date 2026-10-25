#!/bin/sh
# Package dist/ into the release archive (no game data inside).
#   ./build.sh && sh tools/make_release.sh v1.0.0
set -e
cd "$(dirname "$0")/.."
VER=${1:-dev}
NAME=carma2web-$VER
rm -rf "dist/$NAME" && mkdir -p "dist/$NAME/site/music" "dist/$NAME/tools"
cp dist/site/index.html dist/site/config.js dist/site/.htaccess dist/site/carma2hy.js dist/site/carma2hy.wasm "dist/$NAME/site/"
cp docs/music-README.txt "dist/$NAME/site/music/README.txt"
cp -r server "dist/$NAME/server"
cp tools/make_data.py "dist/$NAME/tools/"
cp tools/make_data.cmd "dist/$NAME/tools/" 2>/dev/null || true
cp docs/GAME_FILES.md docs/HOSTING.md "dist/$NAME/"
cp docs/RELEASE_README.txt "dist/$NAME/README.txt"
cp LICENSE "dist/$NAME/"
(cd dist && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME")
echo "dist/$NAME.zip"
