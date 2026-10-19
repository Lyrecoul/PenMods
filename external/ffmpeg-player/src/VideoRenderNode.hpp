#ifndef VIDEO_RENDER_NODE_HPP
#define VIDEO_RENDER_NODE_HPP

// ============================================================
// 视频渲染节点
//
// OpenGL 场景图（Qt5）下按优先级选择：
//   1. DRM_PRIME 帧 → EGLImage(dma-buf) → GL_TEXTURE_EXTERNAL_OES
//      零拷贝，YUV→RGB 由 GPU 采样器完成
//   2. DRM_PRIME 帧 → mmap → NV12 双平面纹理（零拷贝导入失败时回退）
//   3. 软解 YUV420P/NV12 → 平面纹理，着色器内做颜色转换
//   4. 其他像素格式 → sws 转 YUV420P 后走 3
// 纹理复用（glTexSubImage2D），不再每帧新建纹理。
//
// 非 OpenGL 场景图（software/RHI/Qt6）：CPU 转 RGBA，
// 并直接缩放到显示尺寸，减少转换量。
//
// 环境变量 FFPLAYER_ZEROCOPY=0 可禁用路径 1，FFPLAYER_GL=0 强制 CPU 路径。
// ============================================================

#include "DrmPrime.hpp"
#include "Logger.hpp"

#include <QImage>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && !defined(QT_NO_OPENGL)
#define FFPLAYER_GL_RENDER 1
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QVector2D>
#include <dlfcn.h>
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
#include <QSGRendererInterface>
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#else
#define FFPLAYER_GL_RENDER 0
#endif

namespace ffmpeg_player {
namespace render {

inline bool EnvDisabled(const char *name) {
  const char *v = std::getenv(name);
  return v && std::strcmp(v, "0") == 0;
}

inline bool IsFullRange(const AVFrame *f) {
  return f->color_range == AVCOL_RANGE_JPEG ||
         f->format == AV_PIX_FMT_YUVJ420P ||
         f->format == AV_PIX_FMT_YUVJ422P ||
         f->format == AV_PIX_FMT_YUVJ444P ||
         f->format == AV_PIX_FMT_YUVJ440P;
}

inline bool IsBt709(const AVFrame *f) {
  switch (f->colorspace) {
  case AVCOL_SPC_BT709:
    return true;
  case AVCOL_SPC_BT470BG:
  case AVCOL_SPC_SMPTE170M:
  case AVCOL_SPC_FCC:
    return false;
  default:
    return f->height >= 720;
  }
}

// rgb = M * (y, u, v, 1)，y/u/v 为 [0,1] 纹理采样值
inline QMatrix4x4 YuvToRgbMatrix(const AVFrame *f) {
  const bool full = IsFullRange(f);
  const bool bt709 = IsBt709(f);
  const float kr = bt709 ? 0.2126f : 0.299f;
  const float kb = bt709 ? 0.0722f : 0.114f;
  const float kg = 1.0f - kr - kb;
  const float ys = full ? 1.0f : 255.0f / 219.0f;
  const float cs = full ? 1.0f : 255.0f / 224.0f;
  const float yoff = full ? 0.0f : 16.0f / 255.0f;

  const float rv = 2.0f * (1.0f - kr) * cs;
  const float bu = 2.0f * (1.0f - kb) * cs;
  const float gu = 2.0f * (1.0f - kb) * kb / kg * cs;
  const float gv = 2.0f * (1.0f - kr) * kr / kg * cs;
  const float y0 = -ys * yoff;

  return QMatrix4x4(ys, 0.0f, rv, y0 - rv * 0.5f,           //
                    ys, -gu, -gv, y0 + (gu + gv) * 0.5f,    //
                    ys, bu, 0.0f, y0 - bu * 0.5f,           //
                    0.0f, 0.0f, 0.0f, 1.0f);
}

// ─── 旋转/摆放计算 ───────────────────────────────
struct Placement {
  QRectF tex_rect;
  QMatrix4x4 matrix;
};

inline Placement ComputePlacement(const QRectF &rect, int rotation) {
  Placement p;
  if (rotation == 0) {
    p.tex_rect = rect;
    return p;
  }
  const qreal cx = rect.x() + rect.width() / 2.0;
  const qreal cy = rect.y() + rect.height() / 2.0;
  const bool swap = (rotation == 90 || rotation == 270);
  p.tex_rect = swap ? QRectF(cx - rect.height() / 2.0, cy - rect.width() / 2.0,
                             rect.height(), rect.width())
                    : rect;
  p.matrix.translate(static_cast<float>(cx), static_cast<float>(cy), 0);
  p.matrix.rotate(static_cast<float>(rotation), 0, 0, 1);
  p.matrix.translate(static_cast<float>(-cx), static_cast<float>(-cy), 0);
  return p;
}

// ─── CPU 像素转换（sws） ─────────────────────────────
class SwsConverter {
public:
  ~SwsConverter() {
    if (sws_)
      sws_freeContext(sws_);
  }

  // 转换为 RGBA 并缩放到 dst_w x dst_h
  bool ToRgba(const AVFrame *frame, int dst_w, int dst_h, QImage &out) {
    if (out.width() != dst_w || out.height() != dst_h ||
        out.format() != QImage::Format_RGBA8888)
      out = QImage(dst_w, dst_h, QImage::Format_RGBA8888);
    if (out.isNull())
      return false;
    uint8_t *dst[4] = {out.bits(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {static_cast<int>(out.bytesPerLine()), 0, 0, 0};
    return Scale(frame, dst_w, dst_h, AV_PIX_FMT_RGBA, dst, dst_linesize);
  }

  // 转换为同尺寸 YUV420P（GL 路径处理不常见像素格式时使用）
  bool ToI420(const AVFrame *frame, AVFrame *out) {
    if (out->width != frame->width || out->height != frame->height ||
        out->format != AV_PIX_FMT_YUV420P || !out->data[0]) {
      av_frame_unref(out);
      out->width = frame->width;
      out->height = frame->height;
      out->format = AV_PIX_FMT_YUV420P;
      if (av_frame_get_buffer(out, 32) < 0)
        return false;
    }
    out->color_range = IsFullRange(frame) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    out->colorspace = frame->colorspace;
    return Scale(frame, out->width, out->height, AV_PIX_FMT_YUV420P,
                 out->data, out->linesize);
  }

private:
  bool Scale(const AVFrame *frame, int dst_w, int dst_h, AVPixelFormat dst_fmt,
             uint8_t *const dst[], const int dst_linesize[]) {
    const uint8_t *src[4] = {nullptr, nullptr, nullptr, nullptr};
    int src_linesize[4] = {0, 0, 0, 0};
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);

    std::unique_ptr<drm::Nv12Mapping> mapping;
    if (src_fmt == AV_PIX_FMT_DRM_PRIME) {
      drm::Nv12Layout layout;
      if (!drm::GetNv12Layout(frame, layout))
        return false;
      mapping = std::make_unique<drm::Nv12Mapping>(layout);
      if (!mapping->ok())
        return false;
      for (int i = 0; i < 2; ++i) {
        src[i] = mapping->data[i];
        src_linesize[i] = mapping->linesize[i];
      }
      src_fmt = AV_PIX_FMT_NV12;
    } else {
      for (int i = 0; i < 4; ++i) {
        src[i] = frame->data[i];
        src_linesize[i] = frame->linesize[i];
      }
    }

    const bool same_size = dst_w == frame->width && dst_h == frame->height;
    sws_ = sws_getCachedContext(sws_, frame->width, frame->height, src_fmt,
                                dst_w, dst_h, dst_fmt,
                                same_size ? SWS_POINT : SWS_FAST_BILINEAR,
                                nullptr, nullptr, nullptr);
    if (!sws_)
      return false;
    // swscale 默认按 BT.601 处理；每帧更新，避免缓存上下文沿用旧颜色信息。
    const int* coefficients = sws_getCoefficients(IsBt709(frame) ? SWS_CS_ITU709 : SWS_CS_ITU601);
    const int full = IsFullRange(frame) ? 1 : 0;
    sws_setColorspaceDetails(sws_, coefficients, full, coefficients,
                            dst_fmt == AV_PIX_FMT_RGBA ? 1 : full, 0, 1 << 16, 1 << 16);
    return sws_scale(sws_, src, src_linesize, 0, frame->height, dst, dst_linesize) == dst_h;
  }

  SwsContext *sws_ = nullptr;
};

#if FFPLAYER_GL_RENDER

// ─── EGL dma-buf 导入（运行时解析符号，不链接 libEGL） ─────────
class EglDmaBufImporter {
public:
  bool Available(QOpenGLContext *ctx) {
    if (state_ == State::Unknown)
      state_ = Init(ctx) ? State::Ready : State::Disabled;
    return state_ == State::Ready;
  }

  void Disable() {
    if (state_ != State::Disabled) {
      LOG(WARNING) << "dma-buf zero-copy import failed, using mmap upload";
      state_ = State::Disabled;
    }
  }

  void *Create(const drm::Nv12Layout &l, const AVFrame *frame) {
    std::vector<int32_t> attrs = {
        kWidth,         l.width,
        kHeight,        l.height,
        kDrmFourcc,     static_cast<int32_t>(drm::kFourccNV12),
        kPlane0Fd,      l.fd[0],
        kPlane0Offset,  static_cast<int32_t>(l.offset[0]),
        kPlane0Pitch,   static_cast<int32_t>(l.pitch[0]),
        kPlane1Fd,      l.fd[1],
        kPlane1Offset,  static_cast<int32_t>(l.offset[1]),
        kPlane1Pitch,   static_cast<int32_t>(l.pitch[1]),
    };
    const size_t base_size = attrs.size();
    if (use_hints_) {
      attrs.insert(attrs.end(),
                   {kColorSpaceHint, IsBt709(frame) ? kRec709 : kRec601,
                    kSampleRangeHint,
                    IsFullRange(frame) ? kFullRange : kNarrowRange});
    }
    attrs.push_back(kNone);

    void *image = create_image_(display_, nullptr, kLinuxDmaBuf, nullptr,
                                attrs.data());
    if (!image && use_hints_) {
      // 部分驱动不认 YUV hint，去掉后重试
      attrs.resize(base_size);
      attrs.push_back(kNone);
      image = create_image_(display_, nullptr, kLinuxDmaBuf, nullptr,
                            attrs.data());
      if (image)
        use_hints_ = false;
    }
    return image;
  }

  void Destroy(void *image) {
    if (image && destroy_image_)
      destroy_image_(display_, image);
  }

  void BindToBoundTexture(void *image) {
    image_target_texture_(GL_TEXTURE_EXTERNAL_OES, image);
  }

private:
  enum class State { Unknown, Ready, Disabled };

  using Proc = void (*)();
  using GetProcAddressFn = Proc (*)(const char *);
  using GetCurrentDisplayFn = void *(*)();
  using QueryStringFn = const char *(*)(void *, int32_t);
  using CreateImageFn = void *(*)(void *, void *, uint32_t, void *,
                                  const int32_t *);
  using DestroyImageFn = uint32_t (*)(void *, void *);
  using ImageTargetTextureFn = void (*)(uint32_t, void *);

  // EGL_EXT_image_dma_buf_import
  static constexpr int32_t kNone = 0x3038;
  static constexpr int32_t kExtensions = 0x3055;
  static constexpr int32_t kHeight = 0x3056;
  static constexpr int32_t kWidth = 0x3057;
  static constexpr uint32_t kLinuxDmaBuf = 0x3270;
  static constexpr int32_t kDrmFourcc = 0x3271;
  static constexpr int32_t kPlane0Fd = 0x3272;
  static constexpr int32_t kPlane0Offset = 0x3273;
  static constexpr int32_t kPlane0Pitch = 0x3274;
  static constexpr int32_t kPlane1Fd = 0x3275;
  static constexpr int32_t kPlane1Offset = 0x3276;
  static constexpr int32_t kPlane1Pitch = 0x3277;
  static constexpr int32_t kColorSpaceHint = 0x327B;
  static constexpr int32_t kSampleRangeHint = 0x327C;
  static constexpr int32_t kRec601 = 0x327F;
  static constexpr int32_t kRec709 = 0x3280;
  static constexpr int32_t kFullRange = 0x3282;
  static constexpr int32_t kNarrowRange = 0x3283;

  template <typename T> static T Sym(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
  }

  bool Init(QOpenGLContext *ctx) {
    if (EnvDisabled("FFPLAYER_ZEROCOPY"))
      return false;
    if (!ctx->hasExtension("GL_OES_EGL_image_external")) {
      LOG(INFO) << "GL_OES_EGL_image_external missing, zero-copy disabled";
      return false;
    }

    // 只用 EGL 自己的入口：GLX 平台下 eglGetCurrentDisplay 会返回空，自然禁用
    auto get_display = Sym<GetCurrentDisplayFn>("eglGetCurrentDisplay");
    auto query_string = Sym<QueryStringFn>("eglQueryString");
    auto get_proc = Sym<GetProcAddressFn>("eglGetProcAddress");
    if (!get_display || !query_string || !get_proc)
      return false;

    display_ = get_display();
    if (!display_)
      return false;
    const char *exts = query_string(display_, kExtensions);
    if (!exts || !std::strstr(exts, "EGL_EXT_image_dma_buf_import")) {
      LOG(INFO) << "EGL_EXT_image_dma_buf_import missing, zero-copy disabled";
      return false;
    }

    create_image_ =
        reinterpret_cast<CreateImageFn>(get_proc("eglCreateImageKHR"));
    destroy_image_ =
        reinterpret_cast<DestroyImageFn>(get_proc("eglDestroyImageKHR"));
    image_target_texture_ = reinterpret_cast<ImageTargetTextureFn>(
        get_proc("glEGLImageTargetTexture2DOES"));
    if (!create_image_ || !destroy_image_ || !image_target_texture_)
      return false;

    LOG(INFO) << "dma-buf zero-copy rendering enabled";
    return true;
  }

  State state_ = State::Unknown;
  bool use_hints_ = true;
  void *display_ = nullptr;
  CreateImageFn create_image_ = nullptr;
  DestroyImageFn destroy_image_ = nullptr;
  ImageTargetTextureFn image_target_texture_ = nullptr;
};

// ─── 材质与着色器 ────────────────────────────────────
enum class GlVideoMode { I420 = 0, NV12 = 1, External = 2 };

// ── PENMODS PATCH: 缩小采样 ─────────────────────────────────────────────
// 上游只做一次 GL_LINEAR 采样（无 mipmap），视频分辨率高于屏幕时每输出像素只
// 采 2×2 纹素，高频折叠成摩尔纹（1080p→302×170 实测高频能量是正确缩放的 13 倍，
// 观感像被"锐化"）。这里在缩小时按 footprint 做 N×N 抽样。
//   - 抽样次数上限按纹理数分配：External（硬解零拷贝，单纹理）8，
//     NV12 6，I420 5，保证每像素纹理采样数都在 ~75 以内。
//   - 放大（taps<1.5）时着色器直接走原单次采样，行为与上游一致。
// 详见 external/ffmpeg-player/UPSTREAM.md 与 doc/FFMPEG_PLAYER_ANALYSIS.md。
inline int MaxTapsForMode(GlVideoMode mode) {
  switch (mode) {
  case GlVideoMode::External:
    return 8;
  case GlVideoMode::NV12:
    return 6;
  default:
    return 5;
  }
}
// ── PENMODS PATCH end ──────────────────────────────────────────────────

class GlVideoMaterial : public QSGMaterial {
public:
  explicit GlVideoMaterial(GlVideoMode mode, bool multisampling = true)
      : mode_(mode), multisampling_(multisampling) {}

  QSGMaterialType *type() const override {
    static QSGMaterialType types[3][2];
    return &types[static_cast<int>(mode_)][multisampling_ ? 1 : 0];
  }
  QSGMaterialShader *createShader() const override;

  GlVideoMode mode() const { return mode_; }
  bool multisampling() const { return multisampling_; }

  bool smooth = true;
  GLuint textures[3] = {0, 0, 0};
  QVector2D crop[3] = {QVector2D(1, 1), QVector2D(1, 1), QVector2D(1, 1)};
  QMatrix4x4 color_matrix;
  // PENMODS PATCH: 缩小时的多点抽样参数（taps=1 表示不缩小，行为同上游）
  QVector2D tap_step = QVector2D(0, 0);
  float taps = 1.0f;

private:
  GlVideoMode mode_;
  bool multisampling_;
};

class GlVideoShader : public QSGMaterialShader {
public:
  explicit GlVideoShader(GlVideoMode mode, bool multisampling)
      : mode_(mode), multisampling_(multisampling), source_(BuildFragmentShader()) {}

  char const *const *attributeNames() const override {
    static const char *const names[] = {"a_position", "a_texcoord", nullptr};
    return names;
  }

  void updateState(const RenderState &state, QSGMaterial *new_material,
                   QSGMaterial *) override {
    QOpenGLShaderProgram *p = program();
    if (state.isMatrixDirty())
      p->setUniformValue(matrix_loc_, state.combinedMatrix());
    if (state.isOpacityDirty())
      p->setUniformValue(opacity_loc_, state.opacity());

    auto *m = static_cast<GlVideoMaterial *>(new_material);
    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    const GLenum target =
        mode_ == GlVideoMode::External ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    const int planes = PlaneCount();
    // 逆序绑定，结束时活动单元停在 0（Qt 默认假设）
    for (int i = planes - 1; i >= 0; --i) {
      f->glActiveTexture(GL_TEXTURE0 + i);
      f->glBindTexture(target, m->textures[i]);
      const GLint filter = m->smooth ? GL_LINEAR : GL_NEAREST;
      f->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
      f->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
      p->setUniformValue(tex_loc_[i], i);
      p->setUniformValue(crop_loc_[i], m->crop[i]);
    }
    if (mode_ != GlVideoMode::External)
      p->setUniformValue(color_loc_, m->color_matrix);
    // PENMODS PATCH: 缩小时的抽样参数
    if (multisampling_) {
      p->setUniformValue(tap_step_loc_, m->tap_step);
      p->setUniformValue(taps_loc_, m->taps);
    }
  }

protected:
  const char *vertexShader() const override {
    return "attribute highp vec4 a_position;\n"
           "attribute highp vec2 a_texcoord;\n"
           "uniform highp mat4 qt_Matrix;\n"
           "varying highp vec2 v_texcoord;\n"
           "void main() {\n"
           "  v_texcoord = a_texcoord;\n"
           "  gl_Position = qt_Matrix * a_position;\n"
           "}\n";
  }

  // Qt caches these shaders by sampler layout and multi-tap/single-fetch material type.
  const char *fragmentShader() const override {
    return source_.c_str();
  }

  // PENMODS PATCH: 单次采样 vs 缩小时的 N×N 平均
  std::string PlaneTapFunction(const char *ret, const char *name,
                                      const char *sampler, const char *swizzle,
                                      const char *zero) const {
    std::string s;
    s += ret;
    s += " ";
    s += name;
    s += "(";
    s += sampler;
    s += " t, highp vec2 uv, highp vec2 cs) {\n";
    if (!multisampling_) {
      s += std::string("  return texture2D(t, uv * cs)") + swizzle + ";\n}\n";
      return s;
    }
    s += std::string("  if (taps < 1.5) return texture2D(t, uv * cs)") + swizzle + ";\n";
    s += std::string("  ") + ret + " acc = " + zero + ";\n";
    s += "  highp float cnt = 0.0;\n";
    s += "  for (int y = 0; y < MAXTAPS; ++y) {\n";
    s += "    if (float(y) >= taps) break;\n";
    s += "    for (int x = 0; x < MAXTAPS; ++x) {\n";
    s += "      if (float(x) >= taps) break;\n";
    s += "      highp vec2 off = (vec2(float(x), float(y)) - (taps - 1.0) * 0.5) * tap_step;\n";
    s += std::string("      acc += texture2D(t, (uv + off) * cs)") + swizzle + ";\n";
    s += "      cnt += 1.0;\n";
    s += "    }\n";
    s += "  }\n";
    s += "  return acc / cnt;\n";
    s += "}\n";
    return s;
  }

  std::string BuildFragmentShader() const {
    const bool external = (mode_ == GlVideoMode::External);
    const char *sampler = external ? "samplerExternalOES" : "sampler2D";
    std::string s;
    if (external)
      s += "#extension GL_OES_EGL_image_external : require\n";
    s += "#ifdef GL_ES\nprecision highp float;\n#endif\n";
    s += "varying highp vec2 v_texcoord;\n";
    s += "uniform lowp float opacity;\n";
    if (multisampling_) {
      s += "uniform highp vec2 tap_step;\n";
      s += "uniform highp float taps;\n";
      s += "#define MAXTAPS " + std::to_string(MaxTapsForMode(mode_)) + "\n";
    }
    for (int i = 0; i < PlaneCount(); ++i)
      s += std::string("uniform ") + sampler + " tex" + std::to_string(i) + ";\n";
    for (int i = 0; i < PlaneCount(); ++i)
      s += "uniform highp vec2 crop" + std::to_string(i) + ";\n";
    if (!external)
      s += "uniform highp mat4 color_matrix;\n";
    s += PlaneTapFunction("highp float", "planeTap1", "sampler2D", ".r", "0.0");
    if (mode_ == GlVideoMode::NV12)
      s += PlaneTapFunction("highp vec2", "planeTap2", "sampler2D", ".ra", "vec2(0.0)");
    if (external)
      s += PlaneTapFunction("highp vec3", "planeTap3", sampler, ".rgb", "vec3(0.0)");
    s += "void main() {\n";
    switch (mode_) {
    case GlVideoMode::I420:
      s += "  highp vec4 yuv = vec4(planeTap1(tex0, v_texcoord, crop0),\n"
           "                        planeTap1(tex1, v_texcoord, crop1),\n"
           "                        planeTap1(tex2, v_texcoord, crop2),\n"
           "                        1.0);\n";
      break;
    case GlVideoMode::NV12:
      s += "  highp vec4 yuv = vec4(planeTap1(tex0, v_texcoord, crop0),\n"
           "                        planeTap2(tex1, v_texcoord, crop1),\n"
           "                        1.0);\n";
      break;
    case GlVideoMode::External:
      s += "  gl_FragColor = vec4(planeTap3(tex0, v_texcoord, crop0), 1.0) * opacity;\n";
      break;
    }
    if (!external)
      s += "  gl_FragColor = vec4((color_matrix * yuv).rgb, 1.0) * opacity;\n";
    s += "}\n";
    return s;
  }

  void initialize() override {
    static const char *const tex_names[3] = {"tex0", "tex1", "tex2"};
    static const char *const crop_names[3] = {"crop0", "crop1", "crop2"};
    QOpenGLShaderProgram *p = program();
    matrix_loc_ = p->uniformLocation("qt_Matrix");
    opacity_loc_ = p->uniformLocation("opacity");
    color_loc_ = p->uniformLocation("color_matrix");
    tap_step_loc_ = p->uniformLocation("tap_step");
    taps_loc_ = p->uniformLocation("taps");
    for (int i = 0; i < PlaneCount(); ++i) {
      tex_loc_[i] = p->uniformLocation(tex_names[i]);
      crop_loc_[i] = p->uniformLocation(crop_names[i]);
    }
  }

private:
  int PlaneCount() const {
    return mode_ == GlVideoMode::I420 ? 3 : mode_ == GlVideoMode::NV12 ? 2 : 1;
  }

  GlVideoMode mode_;
  bool multisampling_;
  const std::string source_;
  int matrix_loc_ = -1;
  int opacity_loc_ = -1;
  int color_loc_ = -1;
  int tex_loc_[3] = {-1, -1, -1};
  int crop_loc_[3] = {-1, -1, -1};
  // PENMODS PATCH
  int tap_step_loc_ = -1;
  int taps_loc_ = -1;
};

inline QSGMaterialShader *GlVideoMaterial::createShader() const {
  return new GlVideoShader(mode_, multisampling_);
}

// ─── GL 视频节点：纹理上传与生命周期管理 ─────────────────
class GlVideoNode : public QSGGeometryNode {
public:
  GlVideoNode()
      : geometry_(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4) {
    QSGGeometry::updateTexturedRectGeometry(&geometry_, rect_,
                                            QRectF(0, 0, 1, 1));
    setGeometry(&geometry_);
    setFlag(QSGNode::OwnsMaterial, true);
  }

  ~GlVideoNode() override {
    ReleaseHeld(0);
    if (QOpenGLContext *ctx = QOpenGLContext::currentContext()) {
      QOpenGLFunctions *f = ctx->functions();
      for (Plane &p : planes_) {
        if (p.id)
          f->glDeleteTextures(1, &p.id);
      }
      if (oes_texture_)
        f->glDeleteTextures(1, &oes_texture_);
    }
    if (scratch_)
      av_frame_free(&scratch_);
  }

  // 接管 frame 所有权；返回是否成功更新了画面
  bool SetFrame(AVFrame *frame) {
    // PENMODS PATCH: 记录纹理尺寸并按当前 rect 更新抽样参数
    const int tw = frame->width, th = frame->height;
    const bool ok = UploadFrame(frame);
    if (ok) {
      tex_w_ = tw;
      tex_h_ = th;
      UpdateTapParams();
    }
    return ok;
  }

  void SetSampling(bool downsampling, bool smooth) {
    const bool multisampling = downsampling;
    // Texture filtering is independent of multi-tap downsampling.
    if (multisampling_ == multisampling && smooth_ == smooth)
      return;
    multisampling_ = multisampling;
    smooth_ = smooth;
    if (material_) {
      EnsureMaterial(material_->mode());
      UpdateTapParams();
    }
  }

  // PENMODS PATCH: rect / dpr 变化时重算抽样参数（devicePixel 下的显示尺寸）
  void SetRect(const QRectF &rect, qreal dpr) {
    if (rect == rect_ && dpr == dpr_) {
      return;
    }
    rect_ = rect;
    dpr_ = dpr;
    UpdateTapParams();
    QSGGeometry::updateTexturedRectGeometry(&geometry_, rect,
                                            QRectF(0, 0, 1, 1));
    markDirty(QSGNode::DirtyGeometry);
  }

private:
  bool UploadFrame(AVFrame *frame) {
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx) {
      av_frame_free(&frame);
      return false;
    }
    QOpenGLFunctions *f = ctx->functions();

    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
      drm::Nv12Layout layout;
      if (!drm::GetNv12Layout(frame, layout)) {
        av_frame_free(&frame);
        return false;
      }
      if (importer_.Available(ctx) && UploadExternal(f, frame, layout))
        return true; // frame 已由 held_ 持有

      drm::Nv12Mapping mapping(layout);
      const bool ok = mapping.ok() &&
                      UploadNv12(f, frame, mapping.data, mapping.linesize);
      av_frame_free(&frame);
      return ok;
    }

    bool ok = false;
    switch (frame->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
      ok = UploadI420(f, frame);
      break;
    case AV_PIX_FMT_NV12: {
      const uint8_t *data[2] = {frame->data[0], frame->data[1]};
      ok = UploadNv12(f, frame, data, frame->linesize);
      break;
    }
    default:
      if (!scratch_)
        scratch_ = av_frame_alloc();
      ok = scratch_ && converter_.ToI420(frame, scratch_) &&
           UploadI420(f, scratch_);
      break;
    }
    av_frame_free(&frame);
    return ok;
  }

  // PENMODS PATCH: 由纹理尺寸 / 显示尺寸（devicePixel）算出缩小的 N×N 抽样参数。
  // tap_step 是相邻抽样在 UV 上的间距：每输出像素覆盖 1/w 的 UV，N 个抽样铺满它。
  void UpdateTapParams() {
    if (!material_)
      return;
    const qreal w = std::max<qreal>(1.0, rect_.width() * dpr_);
    const qreal h = std::max<qreal>(1.0, rect_.height() * dpr_);
    float taps = 1.0f;
    if (multisampling_ && tex_w_ > 0 && tex_h_ > 0) {
      const double minify =
          std::max(static_cast<double>(tex_w_) / w, static_cast<double>(tex_h_) / h);
      if (minify > 1.02) {
        taps = static_cast<float>(std::min<double>(
            MaxTapsForMode(material_->mode()), std::ceil(minify)));
      }
    }
    material_->taps = taps;
    material_->tap_step =
        QVector2D(static_cast<float>(1.0 / (w * taps)),
                  static_cast<float>(1.0 / (h * taps)));
    // PENMODS PATCH: 参数变化时打一行日志，便于在设备上确认自适应抽样
    const int dw = static_cast<int>(std::lround(w));
    const int dh = static_cast<int>(std::lround(h));
    if (taps != logged_taps_ || tex_w_ != logged_src_w_ || dw != logged_dst_w_) {
      logged_taps_ = taps;
      logged_src_w_ = tex_w_;
      logged_dst_w_ = dw;
      LOG(INFO) << "Video downsample: taps=" << taps << "x" << taps
                << " source=" << tex_w_ << "x" << tex_h_ << " display=" << dw
                << "x" << dh;
    }
    markDirty(QSGNode::DirtyMaterial);
  }

private:
  struct Plane {
    GLuint id = 0;
    int width = 0;
    int height = 0;
    GLenum format = 0;
  };

  struct HeldFrame {
    AVFrame *frame;
    void *image;
  };

  GlVideoMaterial *EnsureMaterial(GlVideoMode mode) {
    if (!material_ || material_->mode() != mode || material_->multisampling() != multisampling_) {
      auto *replacement = new GlVideoMaterial(mode, multisampling_);
      if (material_ && material_->mode() == mode) {
        // A paused frame has no upload to repopulate these after a shader switch.
        std::copy(std::begin(material_->textures), std::end(material_->textures), replacement->textures);
        std::copy(std::begin(material_->crop), std::end(material_->crop), replacement->crop);
        replacement->color_matrix = material_->color_matrix;
      }
      material_ = replacement;
      setMaterial(material_); // OwnsMaterial：旧材质自动删除
    }
    material_->smooth = smooth_;
    markDirty(QSGNode::DirtyMaterial);
    return material_;
  }

  static void InitTexture(QOpenGLFunctions *f, GLenum target, GLuint &id) {
    f->glGenTextures(1, &id);
    f->glBindTexture(target, id);
    f->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  // 按 linesize 整行上传（纹理宽度 = stride），用 crop 裁掉对齐填充，
  // 避免 GLES2 缺少 GL_UNPACK_ROW_LENGTH 时逐行拷贝。
  static QVector2D UploadPlane(QOpenGLFunctions *f, Plane &plane,
                               GLenum format, int bytes_per_texel,
                               const uint8_t *data, int linesize, int width,
                               int height) {
    const bool whole = linesize >= width * bytes_per_texel &&
                       linesize % bytes_per_texel == 0;
    const int tex_w = whole ? linesize / bytes_per_texel : width;

    if (!plane.id)
      InitTexture(f, GL_TEXTURE_2D, plane.id);
    else
      f->glBindTexture(GL_TEXTURE_2D, plane.id);

    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const bool realloc = plane.width != tex_w || plane.height != height ||
                         plane.format != format;
    if (whole) {
      if (realloc)
        f->glTexImage2D(GL_TEXTURE_2D, 0, format, tex_w, height, 0, format,
                        GL_UNSIGNED_BYTE, data);
      else
        f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, height, format,
                           GL_UNSIGNED_BYTE, data);
    } else {
      if (realloc)
        f->glTexImage2D(GL_TEXTURE_2D, 0, format, tex_w, height, 0, format,
                        GL_UNSIGNED_BYTE, nullptr);
      for (int y = 0; y < height; ++y)
        f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, 1, format,
                           GL_UNSIGNED_BYTE, data + y * linesize);
    }
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    plane.width = tex_w;
    plane.height = height;
    plane.format = format;
    // 有填充时收进半个纹素，避免线性过滤采到填充区出现边缘色带
    const float cx = tex_w > width ? (width - 0.5f) / tex_w : 1.0f;
    return QVector2D(cx, 1.0f);
  }

  bool UploadI420(QOpenGLFunctions *f, const AVFrame *frame) {
    if (frame->linesize[0] <= 0 || frame->linesize[1] <= 0 ||
        frame->linesize[2] <= 0)
      return false;
    const int w = frame->width, h = frame->height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    GlVideoMaterial *m = EnsureMaterial(GlVideoMode::I420);
    m->crop[0] = UploadPlane(f, planes_[0], GL_LUMINANCE, 1, frame->data[0],
                             frame->linesize[0], w, h);
    m->crop[1] = UploadPlane(f, planes_[1], GL_LUMINANCE, 1, frame->data[1],
                             frame->linesize[1], cw, ch);
    m->crop[2] = UploadPlane(f, planes_[2], GL_LUMINANCE, 1, frame->data[2],
                             frame->linesize[2], cw, ch);
    for (int i = 0; i < 3; ++i)
      m->textures[i] = planes_[i].id;
    m->color_matrix = YuvToRgbMatrix(frame);
    ReleaseHeld(1);
    return true;
  }

  bool UploadNv12(QOpenGLFunctions *f, const AVFrame *frame,
                  const uint8_t *const data[2], const int linesize[2]) {
    if (linesize[0] <= 0 || linesize[1] <= 0)
      return false;
    const int w = frame->width, h = frame->height;
    GlVideoMaterial *m = EnsureMaterial(GlVideoMode::NV12);
    m->crop[0] = UploadPlane(f, planes_[0], GL_LUMINANCE, 1, data[0],
                             linesize[0], w, h);
    m->crop[1] = UploadPlane(f, planes_[1], GL_LUMINANCE_ALPHA, 2, data[1],
                             linesize[1], (w + 1) / 2, (h + 1) / 2);
    m->textures[0] = planes_[0].id;
    m->textures[1] = planes_[1].id;
    m->color_matrix = YuvToRgbMatrix(frame);
    ReleaseHeld(1);
    return true;
  }

  // 零拷贝：成功时接管 frame（保持 dma-buf 存活直到 GPU 不再使用）
  bool UploadExternal(QOpenGLFunctions *f, AVFrame *frame,
                      const drm::Nv12Layout &layout) {
    void *image = importer_.Create(layout, frame);
    if (!image) {
      importer_.Disable();
      return false;
    }

    if (!oes_texture_)
      InitTexture(f, GL_TEXTURE_EXTERNAL_OES, oes_texture_);
    else
      f->glBindTexture(GL_TEXTURE_EXTERNAL_OES, oes_texture_);

    for (int i = 0; i < 8 && f->glGetError() != GL_NO_ERROR; ++i) {
    }
    importer_.BindToBoundTexture(image);
    if (f->glGetError() != GL_NO_ERROR) {
      importer_.Destroy(image);
      importer_.Disable();
      return false;
    }

    GlVideoMaterial *m = EnsureMaterial(GlVideoMode::External);
    m->textures[0] = oes_texture_;
    m->crop[0] = QVector2D(1, 1);

    held_.push_back({frame, image});
    // 保留最近 2 帧：上一帧可能仍在 GPU 流水线中被采样
    ReleaseHeld(2);
    return true;
  }

  void ReleaseHeld(size_t keep) {
    while (held_.size() > keep) {
      HeldFrame h = held_.front();
      held_.pop_front();
      importer_.Destroy(h.image);
      av_frame_free(&h.frame);
    }
  }

  QSGGeometry geometry_;
  bool multisampling_ = true, smooth_ = true;
  QRectF rect_;
  GlVideoMaterial *material_ = nullptr;
  // PENMODS PATCH: 抽样参数所需的状态
  qreal dpr_ = 1.0;
  int tex_w_ = 0;
  int tex_h_ = 0;
  float logged_taps_ = -1.0f;
  int logged_src_w_ = -1;
  int logged_dst_w_ = -1;
  Plane planes_[3];
  GLuint oes_texture_ = 0;
  EglDmaBufImporter importer_;
  std::deque<HeldFrame> held_;
  SwsConverter converter_;
  AVFrame *scratch_ = nullptr;
};

#endif // FFPLAYER_GL_RENDER

// ─── 根节点：选择渲染后端，处理旋转与摆放 ────────────────
class VideoRootNode : public QSGTransformNode {
public:
  explicit VideoRootNode(QQuickWindow *window) : window_(window) {
#if FFPLAYER_GL_RENDER
    if (DetectGl(window)) {
      gl_node_ = new GlVideoNode;
      appendChildNode(gl_node_);
      LOG(INFO) << "Video renderer: OpenGL YUV";
      return;
    }
#endif
    tex_node_ = new QSGSimpleTextureNode;
    tex_node_->setFiltering(QSGTexture::Linear);
    tex_node_->setOwnsTexture(true);
    appendChildNode(tex_node_);
    LOG(INFO) << "Video renderer: CPU RGBA";
  }

  ~VideoRootNode() override {
    av_frame_free(&cpu_frame_);
  }

  void SetSampling(bool downsampling, bool smooth) {
#if FFPLAYER_GL_RENDER
    if (gl_node_)
      gl_node_->SetSampling(downsampling, smooth);
#endif
    if (downsampling_ != downsampling || smooth_ != smooth) {
      downsampling_ = downsampling;
      smooth_ = smooth;
      cpu_dirty_ = true;
    }
    if (tex_node_) {
      tex_node_->setFiltering(smooth_ ? QSGTexture::Linear : QSGTexture::Nearest);
      tex_node_->markDirty(QSGNode::DirtyMaterial);
    }
  }

  // 接管 frame 所有权
  void SetFrame(AVFrame *frame, const QRectF &rect, int rotation, qreal dpr) {
    dpr_ = dpr;
#if FFPLAYER_GL_RENDER
    if (gl_node_) {
      if (gl_node_->SetFrame(frame))
        has_content_ = true;
      return;
    }
#endif
    // Retain the source, so changing filtering while paused can recover full detail.
    av_frame_free(&cpu_frame_);
    cpu_frame_ = frame;
    cpu_dirty_ = true;
    UpdateCpuTexture(ComputePlacement(rect, rotation).tex_rect);
  }

  bool HasContent() const { return has_content_; }

  void UpdateLayout(const QRectF &rect, int rotation, qreal dpr = 1) {
    dpr_ = dpr;
    const Placement p = ComputePlacement(rect, rotation);
#if FFPLAYER_GL_RENDER
    if (gl_node_)
      gl_node_->SetRect(p.tex_rect, dpr_); // PENMODS PATCH: 多带一个 dpr
#endif
    if (tex_node_) {
      tex_node_->setRect(p.tex_rect);
      UpdateCpuTexture(p.tex_rect);
    }
    setMatrix(p.matrix);
  }

private:
  void UpdateCpuTexture(const QRectF &tex_rect) {
    if (!cpu_frame_)
      return;
    const bool filtered = downsampling_ && smooth_;
    const int dst_w = filtered ? std::clamp(
        static_cast<int>(std::lround(tex_rect.width() * dpr_)), 1, cpu_frame_->width) : cpu_frame_->width;
    const int dst_h = filtered ? std::clamp(
        static_cast<int>(std::lround(tex_rect.height() * dpr_)), 1, cpu_frame_->height) : cpu_frame_->height;
    if (!cpu_dirty_ && image_.width() == dst_w && image_.height() == dst_h)
      return;
    if (converter_.ToRgba(cpu_frame_, dst_w, dst_h, image_)) {
      if (QSGTexture *texture = window_->createTextureFromImage(image_)) {
        tex_node_->setTexture(texture);
        tex_node_->markDirty(QSGNode::DirtyMaterial);
        has_content_ = true;
        cpu_dirty_ = false;
      }
    }
  }

#if FFPLAYER_GL_RENDER
  static bool DetectGl(QQuickWindow *window) {
    if (EnvDisabled("FFPLAYER_GL"))
      return false;
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
    if (QSGRendererInterface *ri = window->rendererInterface()) {
      if (ri->graphicsApi() != QSGRendererInterface::OpenGL)
        return false;
    }
#else
    Q_UNUSED(window);
#endif
    return QOpenGLContext::currentContext() != nullptr;
  }

  GlVideoNode *gl_node_ = nullptr;
#endif
  QQuickWindow *window_;
  qreal dpr_ = 1;
  bool downsampling_ = true, smooth_ = true, cpu_dirty_ = false;
  AVFrame *cpu_frame_ = nullptr;
  QSGSimpleTextureNode *tex_node_ = nullptr;
  SwsConverter converter_;
  QImage image_;
  bool has_content_ = false;
};

} // namespace render
} // namespace ffmpeg_player

#endif
