#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-node16-linux-arm64}"
OUT="dist/mathjax-server"

mkdir -p dist

echo "[1/2] Bundling with esbuild..."
npx esbuild index.js \
    --bundle \
    --platform=node \
    --external:typescript \
    --external:@resvg/resvg-js \
    --outfile=bundle.js

echo "[2/2] Packaging for $TARGET..."
npx pkg . \
    --targets "$TARGET" \
    --output "$OUT"

rm -f bundle.js

echo "Done: $OUT ($(du -sh "$OUT" | cut -f1))"
