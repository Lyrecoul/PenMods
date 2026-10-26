// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 *
 * Experimental OCR backend: replaces the vendor recognition engine (libyocr.so) with
 * PaddleOCR's PP-OCRv5 mobile recognizer running on ncnn.
 *
 * WHY A SHIM INSTEAD OF A HOOK
 *   The recognition seam is `std::string yocr_recognize(const cv::Mat&)` inside libyocr.so.
 *   Both `cv::Mat` and `std::string` are C++ ABI (libstdc++ 6.0.22 / OpenCV 3.4) while
 *   libPenMods.so is built with libc++ (zig), so hooking that symbol from PenMods would mix
 *   incompatible string layouts. libYoudaoStitch.so only imports six symbols from libyocr.so,
 *   so re-implementing that small surface in the vendor's own toolchain is both simpler and
 *   ABI-safe. See doc/PPOCR_BACKEND_ANALYSIS.md.
 *
 * WHAT IS REPLACED / WHAT IS NOT
 *   Only per-line recognition. Detection and line segmentation (`seg_middle_line`,
 *   `SegLine::*`, the `ydet_ncnn` model) live in libYoudaoStitch.so and are untouched, as is
 *   the 150 MB FST language model (the shipped app never enables it anyway).
 *
 * ABI / RUNTIME PITFALLS THAT COST REAL DEBUGGING TIME (do not re-introduce)
 *   1. OpenCV packs the channel count as `cn - 1`: use `((flags >> 3) & 511) + 1`, not
 *      `(flags >> 3) & 511` -- otherwise every CV_8UC3 Mat is rejected.
 *   2. A Mat may be a non-continuous ROI (no CV_CONTINUOUS_FLAG). Always walk rows with
 *      `step.p[0]`; assuming `cols * cn` overruns the buffer and crashes the whole app.
 *   3. The network has 18385 outputs = blank + 18383 dict characters + ONE extra space
 *      class beyond the dict. Dropping that extra class silently concatenates English
 *      words ("HelloWorld"). Map it to an ASCII space.
 *   4. The app already rescales crops to ~48 px before calling us, so the target height is
 *      a second resample. 48 keeps dense Chinese strokes; 32 is ~2x faster. Runtime knob:
 *      /userdisk/ppocr_target_h.
 *   5. The vendor OCR thread re-recognizes the SAME line as the stitched crop grows (68px ->
 *      138px -> ... -> final). Measured on a real multi-line session: 76 recognitions,
 *      45.9 s total, 27% of the time spent on <300px intermediate crops that are superseded
 *      by a wider crop. /userdisk/ppocr_adaptive_h=<16..48> (absent = off, default) shrinks
 *      only those same-line growing crops <=300px; final crops in that session were all
 *      >=308px, so their quality is untouched. Verify with tools/ppocr-backend/regression
 *      before enabling: it cannot distinguish a genuinely short final line from an
 *      intermediate crop, so it is opt-in only.
 */

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include "net.h"
#include "ppocrv5_dict.h"

#define PPOCR_SHIM_LOG "/userdisk/ppocr_shim.log"

// ---- layout-compatible OpenCV 3.4 cv::Mat ----
namespace cv {
struct MatSize { int* p; };
struct MatStep { size_t* p; size_t buf[2]; };
class Mat {
public:
    int flags;
    int dims;
    int rows, cols;
    unsigned char* data;
    const unsigned char* datastart;
    const unsigned char* dataend;
    const unsigned char* datalimit;
    void* allocator;
    void* u;
    MatSize size;
    MatStep step;
};
} // namespace cv

namespace {

std::mutex g_mtx;
ncnn::Net  g_rec;
bool       g_ready = false;
volatile bool g_interrupt = false;

// ── 自适应高度（实验开关，默认关闭） ─────────────────────────────────────────
// 原厂 OCR 线程会对**同一行**反复识别逐渐变宽的裁切图，最后取最宽的一次。
// 实测（build/diag/scan/report.md）：一次多行扫描 76 次识别、总 45.9 s，
// 其中 27% 花在宽度 <300px 的中间裁切图上（这些结果随后被更宽的裁切取代）。
//
// /userdisk/ppocr_adaptive_h 存在且为 16..48 时启用：当本次裁切图是**同一行的
// 连续变宽**、且宽度 <= 300px（实测最终裁切图都 >=308px）时，用该高度识别中间
// 裁切图；其它情况仍用正常 target_h。默认关闭（文件不存在 = 行为与以前完全一致），
// 因为对「异常短行」无法在线区分中间裁切与最终裁切，必须用样张回归验证。
struct CallState {
    bool      valid = false;
    int       w = 0;
    int       h = 0;
    long long t_ms = 0;
};
CallState g_prev;

long long mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int read_int_file(const char* path, int fallback, int lo, int hi) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fallback;
    char b[16] = {0};
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return fallback;
    int v = atoi(b);
    return (v >= lo && v <= hi) ? v : fallback;
}

void logf(const char* fmt, ...) {
    char buf[1024];
    int n = 0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tmv;
        localtime_r(&ts.tv_sec, &tmv);
        n += snprintf(buf + n, sizeof(buf) - n, "[%02d:%02d:%02d.%03d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ts.tv_nsec / 1000000));
    }
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(buf) - 1) buf[n++] = '\n';
    int fd = open(PPOCR_SHIM_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; close(fd); }
}

const char* kParam = "/userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.param";
const char* kBin   = "/userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.bin";

bool init_locked() {
    if (g_ready) return true;
    g_rec.opt.num_threads = 4;
    g_rec.opt.use_fp16_packed = false;
    g_rec.opt.use_fp16_storage = false;
    g_rec.opt.use_fp16_arithmetic = false;
    if (g_rec.load_param(kParam) != 0) { logf("init: load_param(%s) FAILED", kParam); return false; }
    if (g_rec.load_model(kBin) != 0)   { logf("init: load_model(%s) FAILED", kBin); return false; }
    g_ready = true;
    logf("init: PP-OCRv5 mobile rec ready (dict=%d, threads=4)", character_dict_size);
    return true;
}

std::string decode(const ncnn::Mat& out) {
    std::string text;
    int last = 0;
    for (int i = 0; i < out.h; i++) {
        const float* p = out.row(i);
        int idx = 0; float best = -1e9f;
        for (int j = 0; j < out.w; j++) if (p[j] > best) { best = p[j]; idx = j; }
        if (idx == last) continue;
        last = idx;
        if (idx <= 0) continue;
        const int di = idx - 1;
        if (di < character_dict_size) {
            text += character_dict[di];
        } else if (!text.empty() && text.back() != ' ') {
            // PP-OCR appends the space character as one extra class beyond the dict
            // (18385 outputs = blank + 18383 chars + space). Dropping it concatenates
            // English words, so map it to an ASCII space.
            text += ' ';
        }
    }
    return text;
}

} // namespace

__attribute__((constructor)) static void shim_loaded(void) {
    logf("=== libyocr shim (PP-OCRv5/ncnn) loaded, pid=%d ===", (int)getpid());
}

// ---- the 6 symbols libYoudaoStitch.so imports ----
bool yocr_init_dictpen_model(std::string dir, std::string lang) {
    std::lock_guard<std::mutex> lk(g_mtx);
    logf("yocr_init_dictpen_model(dir='%s', lang='%s')", dir.c_str(), lang.c_str());
    (void)dir; (void)lang;
    return init_locked();          // caller inspects w0: must be non-zero on success
}

void yocr_enable_lang_model(std::string a, std::string b) { (void)a; (void)b; }

void yocr_release_dictpen_model() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_rec.clear();
    g_ready = false;
    logf("yocr_release_dictpen_model()");
}

bool yocr_get_running_interrupt() { return g_interrupt; }

void yocr_set_running_interrupt(bool v) {
    g_interrupt = v;
    logf("yocr_set_running_interrupt(%d)", (int)v);
}

std::string yocr_recognize(const cv::Mat& m) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_interrupt = false;

    const int type = m.flags & 0xfff;
    const int depth = type & 7;
    const int cn = ((type >> 3) & 0x1ff) + 1;   // OpenCV stores cn-1
    if (!m.data || m.dims != 2 || m.rows <= 0 || m.cols <= 0 || depth != 0 || (cn != 3 && cn != 1)
        || m.rows > 4096 || m.cols > 8192) {
        logf("recognize: unsupported Mat flags=%#x dims=%d rows=%d cols=%d cn=%d data=%p",
             m.flags, m.dims, m.rows, m.cols, cn, (void*)m.data);
        return std::string();
    }
    if (!g_ready && !init_locked()) return std::string();

    const int w = m.cols, h = m.rows;
    // runtime-tunable target height (accuracy/latency tradeoff); 48 = PP-OCRv5's training height
    int target_h = read_int_file("/userdisk/ppocr_target_h", 32, 16, 64);
    int adaptive_h = read_int_file("/userdisk/ppocr_adaptive_h", 0, 0, 48);

    // 判断是否「同一行在继续变宽」：高度基本不变、宽度严格变大、距上次调用 <4 s。
    const long long now = mono_ms();
    const bool same_line = g_prev.valid && h <= g_prev.h + (g_prev.h / 4 + 6) && h >= g_prev.h - (g_prev.h / 4 + 6)
        && w > g_prev.w && (now - g_prev.t_ms) <= 4000;
    // 只有宽度 <=300px 的中间裁切图降档（实测最终裁切图都 >=308px），
    // 并且必须真的在变宽（w/prev_w >= 1.2），避免把短的最终行当成中间帧。
    const bool adaptive_shrink = adaptive_h > 0 && same_line && w <= 300 && g_prev.w >= 40
        && (float)w >= (float)g_prev.w * 1.2f && adaptive_h < target_h;
    if (adaptive_shrink) {
        logf("adaptive: %dx%d (prev %dx%d) -> h=%d (target %d, same line)", w, h, g_prev.w, g_prev.h, adaptive_h, target_h);
        target_h = adaptive_h;
    }

    int target_w = (int)((float)w * target_h / h + 0.5f);
    if (target_w < 8) target_w = 8;

    // Always gather into a contiguous buffer using the row stride: the Mat may be a
    // non-continuous ROI (flags without CV_CONTINUOUS_FLAG) and rows would otherwise be
    // read with the wrong stride, overrunning the buffer and taking the app down.
    const size_t stride = (m.step.p && m.step.p[0] > 0) ? (size_t)m.step.p[0] : (size_t)w * cn;
    std::vector<unsigned char> packed((size_t)w * h * 3);
    if (cn == 3) {
        for (int y = 0; y < h; y++) memcpy(&packed[(size_t)y * w * 3], m.data + (size_t)y * stride, (size_t)w * 3);
    } else {
        for (int y = 0; y < h; y++) {
            const unsigned char* row = m.data + (size_t)y * stride;
            unsigned char* dst = &packed[(size_t)y * w * 3];
            for (int x = 0; x < w; x++) { const unsigned char v = row[x]; dst[x * 3] = v; dst[x * 3 + 1] = v; dst[x * 3 + 2] = v; }
        }
    }
    const unsigned char* src = packed.data();

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(src, ncnn::Mat::PIXEL_RGB2BGR, w, h, target_w, target_h);
    const float mean[3] = {127.5f, 127.5f, 127.5f};
    const float norm[3] = {1.f / 127.5f, 1.f / 127.5f, 1.f / 127.5f};
    in.substract_mean_normalize(mean, norm);

    ncnn::Extractor ex = g_rec.create_extractor();
    ex.input("in0", in);
    ncnn::Mat out;
    ex.extract("out0", out);
    std::string text = decode(out);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (adaptive_shrink) {
        logf("recognize[adaptive]: %dx%d cn=%d -> %dx%d, %.1f ms, text=\"%s\"", w, h, cn, target_w, target_h, ms, text.c_str());
    } else {
        logf("recognize: %dx%d cn=%d -> %dx%d, %.1f ms, text=\"%s\"", w, h, cn, target_w, target_h, ms, text.c_str());
    }
    g_prev.valid = true;
    g_prev.w = w;
    g_prev.h = h;
    g_prev.t_ms = now;
    return text;
}
