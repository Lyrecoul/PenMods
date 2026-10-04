#!/bin/sh
# Cross-build the experimental PP-OCR backend for the pen (aarch64, glibc 2.27).
#
# Produces, under tools/ppocr-backend/build/:
#   libyocr.so    drop-in replacement for the vendor recognition engine
#   ab_test       vendor-vs-PP-OCR comparison harness (evaluation only)
#
# TOOLCHAIN (important)
#   Uses aarch64-linux-gnu-g++ -- NOT zig, which PenMods uses for libPenMods.so.
#   Two reasons:
#     1. ABI: the shim must return std::string / read cv::Mat across the libstdc++
#        ABI that libYoudaoStitch.so was built with (GCC 6.x, libstdc++ 6.0.22).
#     2. OpenMP: every ncnn layer threads with `#pragma omp parallel for`. zig's
#        -fopenmp defines _OPENMP but ships no omp.h/runtime, so ncnn would run
#        single-threaded -- measured 3.8x slower.
#   The cross toolchain's sysroot must be glibc 2.27 to match the device.
#
# Run scripts/fetch-deps.sh first.

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${CACHE:-$here/.cache}
BUILD=$here/build
NCNN=$CACHE/ncnn
MODELS=$CACHE/models

CXX=${CXX:-aarch64-linux-gnu-g++}
CC=${CC:-aarch64-linux-gnu-gcc}

command -v "$CXX" >/dev/null || { echo "error: $CXX not found (need an aarch64 cross GCC with OpenMP)"; exit 1; }
[ -d "$NCNN" ] || { echo "error: run scripts/fetch-deps.sh first ($NCNN missing)"; exit 1; }
[ -s "$MODELS/PP_OCRv5_mobile_rec.ncnn.param" ] || { echo "error: run scripts/fetch-deps.sh first"; exit 1; }

mkdir -p "$BUILD"

"$CXX" -print-file-name=omp.h >/dev/null
if [ ! -f "$("$CXX" -print-file-name=include)/omp.h" ]; then
    echo "error: no omp.h in the cross toolchain -- ncnn would lose all threading"; exit 1
fi

# CMAKE_POLICY_VERSION_MINIMUM works around ncnn's old cmake_minimum_required on CMake >= 4.
cmake -S "$NCNN" -B "$NCNN/build-pen" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DNCNN_VULKAN=OFF -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_OPENMP=ON -DNCNN_THREADS=ON \
    -DNCNN_ARM82=OFF -DNCNN_ARM82DOT=OFF -DNCNN_SHARED_LIB=OFF -DNCNN_SIMPLEOCV=ON \
    > "$BUILD/ncnn-cmake.log" 2>&1 || { tail -20 "$BUILD/ncnn-cmake.log"; exit 1; }
ninja -C "$NCNN/build-pen" libncnn.a > "$BUILD/ncnn-build.log" 2>&1 || { tail -20 "$BUILD/ncnn-build.log"; exit 1; }
echo "==> ncnn: $NCNN/build-pen/src/libncnn.a"

INCS="-I$here/src -I$NCNN/src -I$NCNN/build-pen/src"
LABEL="$(date +%Y%m%d)-$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo nogn)"

echo "==> libyocr.so (shim)"
"$CXX" -O2 -std=c++11 -fopenmp -shared -fPIC \
    "$here/src/yocr_shim.cpp" -o "$BUILD/libyocr.so" \
    $INCS "$NCNN/build-pen/src/libncnn.a" \
    -Wl,-soname,libyocr.so -Wl,--build-id=none \
    -lpthread -lm

# Optional: the evaluation harness needs the vendor libraries to call the original engine.
# Pull them off a device with:
#   adb pull /oem/YoudaoDictPen/output/aarch64_libs/libYoudaoStitch.so \\
#            /oem/YoudaoDictPen/output/aarch64_libs/libyocr.so   vendor/
VENDOR=${VENDOR_STITCH:-$here/vendor/libYoudaoStitch.so}
VENDOR_YOCR=${VENDOR_YOCR:-$here/vendor/libyocr.so}
if [ -f "$VENDOR" ] && [ -f "$VENDOR_YOCR" ]; then
    echo "==> ab_test (evaluation harness)"
    "$CXX" -O2 -std=c++11 -fopenmp "$here/src/ab_test.cpp" -o "$BUILD/ab_test" \
        $INCS "$NCNN/build-pen/src/libncnn.a" "$VENDOR" "$VENDOR_YOCR" \
        -Wl,--allow-shlib-undefined -lpthread -lm -ldl
else
    echo "==> ab_test skipped (put libYoudaoStitch.so + libyocr.so in $here/vendor/)"
fi

# The shim is useless (and dangerous) if it does not export exactly what the vendor
# libYoudaoStitch.so imports, so verify that here instead of on the device.
echo "==> symbol check"
want='yocr_recognize[abi:cxx11](cv::Mat const&)|yocr_init_dictpen_model(std::string, std::string)|yocr_enable_lang_model(std::string, std::string)|yocr_release_dictpen_model()|yocr_get_running_interrupt()|yocr_set_running_interrupt(bool)'
got=$(nm -D --defined-only "$BUILD/libyocr.so" | awk '$2=="T"{print $3}' | c++filt | sort)
echo "$got" | sed 's/^/    /'
n=$(echo "$got" | grep -c '^yocr_')
[ "$n" = 6 ] || { echo "error: expected 6 yocr_* exports, got $n"; exit 1; }

# Guard against deploying something the device cannot load.
bad=$(readelf --version-info "$BUILD/libyocr.so" | grep -oE 'GLIBC_2\.[0-9]+' | sort -u | awk -F. '$2+0 > 27')
if [ -n "$bad" ]; then
    echo "warning: shim requires newer glibc than the device (2.27): $bad"
fi

echo "==> built (label $LABEL)"
ls -la "$BUILD"
