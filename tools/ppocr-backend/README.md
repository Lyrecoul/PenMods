# Experimental OCR backend (PP-OCRv5 + ncnn)

Replaces the pen's **line recognition** stage with PaddleOCR's PP-OCRv5 mobile recognizer
running on ncnn. Everything else in the scan pipeline (camera, stitching, line
detection/segmentation) is untouched.

> **Status: experimental, opt-in, off by default.** It swaps a vendor `.so` at load time via
> `LD_LIBRARY_PATH` shadowing. It is not part of the xmake build, ships no installer and is
> not covered by the OTA. See `doc/PPOCR_BACKEND_ANALYSIS.md` for the reverse engineering and
> the measured numbers.

## Why it exists

The shipped engine (`/oem/YoudaoDictPen/output/aarch64_libs/libyocr.so`) is a CRNN + a 150 MB
FST language model. On real pen scans of Chinese textbook pages it garbles long lines; in our
A/B it turned `学员凭此承诺书进入相关科目考试场，在应考技能考试科目` into
`学员赁进进推的能进科目`. PP-OCRv5 mobile reads those lines correctly at the cost of latency
(roughly 0.2–2 s per line depending on height and length, vs ~0.25 s for the vendor).

## How it works

`libYoudaoStitch.so` imports exactly six symbols from `libyocr.so`, the interesting one being

```cpp
std::string yocr_recognize(const cv::Mat& lineCrop);
```

`cv::Mat` and `std::string` are libstdc++/OpenCV-3.4 C++ ABI, while `libPenMods.so` is built
with libc++ (zig), so a PenMods hook cannot take that seam safely. Instead we re-implement the
six symbols in a shim built with the *vendor's* toolchain — the same GCC 6.x / glibc 2.27 /
libstdc++ 6.0.22 combination the vendor libraries use — and let it shadow the real library.
The shim runs PP-OCRv5 mobile rec (as an ncnn model) on the line crop it is handed.

## Install (the simple path)

```sh
scripts/fetch-deps.sh && scripts/build.sh && scripts/make-bundle.sh
# -> dist/penmods-ppocr.sh, a single self-contained file (~8 MB: shim + model)
```

Then, on the device, push that one file and run it:

```sh
adb push dist/penmods-ppocr.sh /userdisk/
adb shell 'sh /userdisk/penmods-ppocr.sh'
```

It unpacks the payload, stages it, enables the shim, verifies with the dynamic loader that
`libyocr.so` resolves to the shim and that nothing is missing, restarts the app and reports the
result. The same file then manages everything:

| Command | Effect |
|---|---|
| `sh penmods-ppocr.sh` | install + enable (default) |
| `sh penmods-ppocr.sh status` | what is installed / active, plus the last recognitions |
| `sh penmods-ppocr.sh disable` | back to the vendor engine (payload stays staged) |
| `sh penmods-ppocr.sh enable` | enable again |
| `sh penmods-ppocr.sh h 32` | target height 16..64 (48 = most accurate, applies immediately) |
| `sh penmods-ppocr.sh remove` | uninstall everything |

`enable`/`disable` restart the app, because `libyocr.so` is resolved by the dynamic loader at
process start — the guardian relaunches it within a few seconds.

## In-app switch

After the payload is installed, the backend is also reachable from the mod's own settings:
**更多设置 → 系统调整 → 实验性功能**.

| Row | Effect |
|---|---|
| 使用 PP-OCRv5 识别引擎 | enable/disable. Swaps the loader-shadow file and restarts the app (a few seconds), because `libyocr.so` is resolved by the loader at process start. Shown disabled with a hint when the payload is missing. |
| 识别精度优先（较慢） | target height 48 vs 32 — applies to the next recognized line, no restart. |

It is backed by `mod::OcrBackend` (`src/tweaker/OcrBackend.cpp`, context property `ocrBackend`)
and reads exactly the same files the shim does (`/userdisk/ppocr_backend`,
`/userdisk/Qtlib/libyocr.so`, `/userdisk/ppocr_target_h`), so the shell path and the UI never
disagree and there is no second source of truth to keep in sync.

## Build

```sh
scripts/fetch-deps.sh     # ncnn 20240820 + PP-OCRv5 mobile rec (cached in .cache/, untracked)
scripts/build.sh          # -> build/libyocr.so (+ build/ab_test)
scripts/make-bundle.sh    # -> dist/penmods-ppocr.sh
```

Requirements: an `aarch64-linux-gnu-g++` cross toolchain **with OpenMP and a glibc 2.27
sysroot** (on Arch: `aarch64-linux-gnu-gcc`). zig will not do: ncnn threads every layer with
`#pragma omp parallel for`, and zig's `-fopenmp` has no `omp.h`/runtime, which makes the whole
model run single-threaded (measured 3.8x slower).

## Developer path (`scripts/deploy.sh`)

`scripts/deploy.sh` does the same thing straight from the build tree, which is handier while
iterating; it additionally checks that a fixed 32-byte `abi`/`structSize` surface still matches.
The one-file installer is what users should get.

| Command | Effect |
|---|---|
| `scripts/deploy.sh on [h]` | push build output + models, enable, restart |
| `scripts/deploy.sh off` | revert to the vendor engine |
| `scripts/deploy.sh h <n>` / `status` / `log` | knob / state / per-line log |

Installed files, all on `/userdisk` (nothing in `/oem`/system is modified):

| Path | Purpose |
|---|---|
| `/userdisk/Qtlib/libyocr.so` | the shim; first entry of the app's `LD_LIBRARY_PATH` shadows the vendor lib |
| `/userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.{param,bin}` | model (8.2 MB) |
| `/userdisk/ppocr_target_h` | target height, 16..64 (default 32) |
| `/userdisk/ppocr_shim.log` | per-call log: input/output dims, ms, text |

Reverting is `rm /userdisk/Qtlib/libyocr.so` + restart the app.

## Accuracy / latency trade-off

`/userdisk/ppocr_target_h` is the main knob. The app already rescales each line crop to ~48 px
before calling the recognizer, so `48` means "no second resample" and anything lower is a
second downscale.

| target_h | long line (~1400×50) | dense-Chinese accuracy |
|---|---|---|
| 48 | ~1.9–2.1 s | best — keeps 流程控制结构 etc. |
| 40 | ~1.6 s | in between |
| 32 | ~0.9–1.0 s | loses strokes on dense Chinese (`流程控制结构` → `流程介绍`) |
| 24 | ~0.3 s | unusable on long lines |

English is insensitive to the height (letters are well separated) and reads correctly at 32.

## Evaluation harness

`build/ab_test` runs the vendor engine and PP-OCRv5 on the *same* `cv::Mat` and prints both
texts and timings. Run it with the vendor library in the loader path (i.e. with the shim
**off**), or it will compare the shim against itself:

```sh
scripts/deploy.sh off
adb push build/ab_test /userdisk/ && adb push some.ppm /userdisk/
adb shell 'cd /userdisk && LD_LIBRARY_PATH=/oem/YoudaoDictPen/output/libs \
           ./ab_test ocr_model_pro ocr_model /userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.param \
           /userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.bin /userdisk/some.ppm'
```

## Pitfalls already paid for

1. OpenCV packs channels as `cn - 1` (`((flags >> 3) & 511) + 1`); the naive form rejects every
   `CV_8UC3` crop.
2. Crops can be non-continuous ROIs — walk rows with `step.p[0]`, or the app segfaults.
3. The network has one extra output class beyond the dictionary: the space. Dropping it
   concatenates English words.
4. Never `adb push` over `/userdisk/Qtlib/libyocr.so` while the app runs; push elsewhere and
   `mv` (see `deploy.sh`).
5. The cross toolchain must match glibc 2.27, and the shim must link libstdc++ **dynamically**
   so it shares the app's copy — a statically linked libstdc++ would make `std::string` cross
   two incompatible runtimes.
