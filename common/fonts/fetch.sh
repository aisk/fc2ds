#!/bin/sh
# Download the fonts used to build text (not committed, see README.md here).
set -eu

VERSION=2026.10.25
ZIP=fusion-pixel-font-12px-proportional-bdf-v$VERSION.zip
URL=https://github.com/TakWolf/fusion-pixel-font/releases/download/$VERSION/$ZIP
SHA256=23165113b96091b95983635cd6baecd86a6113399a72be6dcaf1445bc1f86b94

cd "$(dirname "$0")"
mkdir -p fusion-pixel
cd fusion-pixel
if [ ! -f "$ZIP" ]; then
    curl -fL -o "$ZIP.part" "$URL"
    mv "$ZIP.part" "$ZIP"
fi
echo "$SHA256  $ZIP" | sha256sum -c -
unzip -o -q "$ZIP" OFL.txt 'LICENSES/*' \
    fusion-pixel-12px-proportional-zh_hans.bdf \
    fusion-pixel-12px-proportional-ja.bdf
echo "fonts ready in $(pwd)"
