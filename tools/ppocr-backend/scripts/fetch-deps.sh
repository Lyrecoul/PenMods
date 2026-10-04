#!/bin/sh
# Fetch the third-party sources/models the experimental PP-OCR backend needs.
#
#   NCNN       : runtime, pinned to a known-good tag
#   PP-OCRv5   : PaddleOCR mobile recognizer, already converted to ncnn by ncnn's maintainer
#                (nihui/ncnn-android-ppocrv5, BSD-3-Clause). The character dictionary that
#                ships next to it lives in ../src/ppocrv5_dict.h.
#
# Everything is cached under $CACHE (default tools/ppocr-backend/.cache), which is meant to
# stay untracked.

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${CACHE:-$here/.cache}
NCNN_TAG=${NCNN_TAG:-20240820}

MODEL_BASE=https://raw.githubusercontent.com/nihui/ncnn-android-ppocrv5/master/app/src/main/assets
REC_PARAM=PP_OCRv5_mobile_rec.ncnn.param
REC_BIN=PP_OCRv5_mobile_rec.ncnn.bin

mkdir -p "$CACHE/models"

echo "==> ncnn $NCNN_TAG"
if [ ! -d "$CACHE/ncnn/.git" ]; then
    git clone --depth 1 --branch "$NCNN_TAG" https://github.com/Tencent/ncnn.git "$CACHE/ncnn"
else
    echo "    cached"
fi

echo "==> PP-OCRv5 mobile rec models"
for f in "$REC_PARAM" "$REC_BIN"; do
    if [ ! -s "$CACHE/models/$f" ]; then
        curl -fsSL --retry 3 -o "$CACHE/models/$f" "$MODEL_BASE/$f"
    fi
    printf '    %-34s %10s bytes  sha256=%s\n' "$f" "$(stat -c%s "$CACHE/models/$f")" \
        "$(sha256sum "$CACHE/models/$f" | cut -d' ' -f1)"
done

echo "==> done, cache at $CACHE"
