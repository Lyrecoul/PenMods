// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 *
 * Evaluation harness: run the vendor recognition engine (libyocr.so) and PP-OCRv5 mobile rec
 * on the SAME cv::Mat line crop, printing both texts and timings.
 *
 *   ab_test <vendor_variant> <vendor_dir> <rec.param> <rec.bin> <line.ppm>...
 *   e.g. ab_test ocr_model_pro ocr_model /userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.param \
 *                 /userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.bin /userdisk/line.ppm
 *
 * It must run with the VENDOR libyocr.so in the loader path (deploy.sh off, or an
 * LD_LIBRARY_PATH that does not contain /userdisk/Qtlib), otherwise both columns are the shim.
 * It chdir()s to the app directory because the vendor initialiser opens its models by relative
 * path, and passes absolute paths for everything else. See tools/ppocr-backend/README.md.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>

#include "net.h"
#include "ppocrv5_dict.h"

// layout-compatible OpenCV 3.4 cv::Mat
#define CV_MAGIC 0x42FF0000
namespace cv {
struct MatSize { int* p; };
struct MatStep { size_t* p; size_t buf[2]; };
class Mat {
public:
    int flags; int dims; int rows, cols;
    unsigned char* data;
    const unsigned char* datastart, *dataend, *datalimit;
    void* allocator; void* u;
    MatSize size; MatStep step;
    void bind(unsigned char* px, int r, int c) {
        flags = CV_MAGIC | 0x4000 | 16; dims = 2; rows = r; cols = c;
        data = px; datastart = data; dataend = data + (size_t)r * c * 3; datalimit = dataend;
        allocator = nullptr; u = nullptr;
        size.p = &rows;
        step.p = step.buf; step.buf[0] = (size_t)c * 3; step.buf[1] = 3;
    }
};
} // namespace cv

extern std::string yocr_recognize(const cv::Mat&);
extern "C" void ydstitch_init(void);
extern "C" void ydstitch_load_ocr(std::string, std::string);
extern "C" void ydstitch_uninit(void);

static double now() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static bool read_ppm(const char* p, std::vector<unsigned char>& rgb, int& w, int& h) {
    FILE* f = fopen(p, "rb"); if (!f) return false;
    char m[3] = {0}; if (fscanf(f, "%2s", m) != 1 || strcmp(m, "P6")) { fclose(f); return false; }
    int c, maxv;
    auto skip = [&] { while ((c = fgetc(f)) != EOF) { if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { ungetc(c, f); break; } } };
    skip(); if (fscanf(f, "%d", &w) != 1) { fclose(f); return false; }
    skip(); if (fscanf(f, "%d", &h) != 1) { fclose(f); return false; }
    skip(); if (fscanf(f, "%d", &maxv) != 1) { fclose(f); return false; }
    fgetc(f);
    rgb.resize((size_t)w * h * 3);
    size_t n = fread(rgb.data(), 1, rgb.size(), f);
    fclose(f);
    return n == rgb.size();
}

static ncnn::Net g_rec;
static bool g_rec_ready = false;

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s <vendor_dir> <vendor_lang> <rec.param> <rec.bin> <img.ppm>...\n", argv[0]); return 2; }

    if (chdir("/oem/YoudaoDictPen/output") != 0) fprintf(stderr, "chdir failed\n");
    ydstitch_init();
    fprintf(stderr, ">>> ydstitch_load_ocr(\"%s\", \"%s\")\n", argv[1], argv[2]);
    ydstitch_load_ocr(std::string(argv[1]), std::string(argv[2]));
    fflush(stderr);

    g_rec.opt.num_threads = 4;
    g_rec.opt.use_fp16_storage = false; g_rec.opt.use_fp16_packed = false; g_rec.opt.use_fp16_arithmetic = false;
    if (g_rec.load_param(argv[3]) || g_rec.load_model(argv[4])) { fprintf(stderr, "ncnn load failed\n"); return 2; }
    g_rec_ready = true;

    for (int a = 5; a < argc; a++) {
        std::vector<unsigned char> rgb; int w = 0, h = 0;
        if (!read_ppm(argv[a], rgb, w, h)) { fprintf(stderr, "%s: read failed\n", argv[a]); continue; }
        cv::Mat m; m.bind(rgb.data(), h, w);

        double t0 = now(); std::string tv = yocr_recognize(m); double t1 = now();

        int target_h = 48, target_w = (int)((float)w * target_h / h + 0.5f);
        double t2 = now();
        ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb.data(), ncnn::Mat::PIXEL_RGB2BGR, w, h, target_w, target_h);
        const float mean[3] = {127.5f, 127.5f, 127.5f}, norm[3] = {1.f/127.5f, 1.f/127.5f, 1.f/127.5f};
        in.substract_mean_normalize(mean, norm);
        ncnn::Extractor ex = g_rec.create_extractor();
        ex.input("in0", in);
        ncnn::Mat out; ex.extract("out0", out);
        std::string tp; int last = 0;
        for (int i = 0; i < out.h; i++) {
            const float* p = out.row(i); int idx = 0; float best = -1e9f;
            for (int j = 0; j < out.w; j++) if (p[j] > best) { best = p[j]; idx = j; }
            if (idx == last) continue; last = idx;
            if (idx <= 0) continue; int di = idx - 1;
            if (di < character_dict_size) tp += character_dict[di];
            else if (!tp.empty() && tp.back() != ' ') tp += ' ';   // extra space class beyond the dict
        }
        double t3 = now();
        printf("%-14s vendor[%6.1fms]: %s\n", strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a], (t1-t0)*1000, tv.c_str());
        printf("%-14s ours  [%6.1fms]: %s\n", "", (t3-t2)*1000, tp.c_str());
    }
    return 0;
}
