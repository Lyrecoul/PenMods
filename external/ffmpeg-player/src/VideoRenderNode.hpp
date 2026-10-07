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

class GlVideoMaterial : public QSGMaterial {
public:
  explicit GlVideoMaterial(GlVideoMode mode) : mode_(mode) {}

  QSGMaterialType *type() const override {
    static QSGMaterialType types[3];
    return &types[static_cast<int>(mode_)];
  }
  QSGMaterialShader *createShader() const override;

  GlVideoMode mode() const { return mode_; }

  GLuint textures[3] = {0, 0, 0};
  QVector2D crop[3] = {QVector2D(1, 1), QVector2D(1, 1), QVector2D(1, 1)};
  QMatrix4x4 color_matrix;

private:
  GlVideoMode mode_;
};

class GlVideoShader : public QSGMaterialShader {
public:
  explicit GlVideoShader(GlVideoMode mode) : mode_(mode) {}

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
      p->setUniformValue(tex_loc_[i], i);
      p->setUniformValue(crop_loc_[i], m->crop[i]);
    }
    if (mode_ != GlVideoMode::External)
      p->setUniformValue(color_loc_, m->color_matrix);
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

  const char *fragmentShader() const override {
    switch (mode_) {
    case GlVideoMode::I420:
      return "#ifdef GL_ES\n"
             "precision highp float;\n"
             "#endif\n"
             "varying highp vec2 v_texcoord;\n"
             "uniform sampler2D tex0;\n"
             "uniform sampler2D tex1;\n"
             "uniform sampler2D tex2;\n"
             "uniform highp vec2 crop0;\n"
             "uniform highp vec2 crop1;\n"
             "uniform highp vec2 crop2;\n"
             "uniform highp mat4 color_matrix;\n"
             "uniform lowp float opacity;\n"
             "void main() {\n"
             "  highp vec4 yuv = vec4(texture2D(tex0, v_texcoord * crop0).r,\n"
             "                        texture2D(tex1, v_texcoord * crop1).r,\n"
             "                        texture2D(tex2, v_texcoord * crop2).r,\n"
             "                        1.0);\n"
             "  gl_FragColor = vec4((color_matrix * yuv).rgb, 1.0) * opacity;\n"
             "}\n";
    case GlVideoMode::NV12:
      return "#ifdef GL_ES\n"
             "precision highp float;\n"
             "#endif\n"
             "varying highp vec2 v_texcoord;\n"
             "uniform sampler2D tex0;\n"
             "uniform sampler2D tex1;\n"
             "uniform highp vec2 crop0;\n"
             "uniform highp vec2 crop1;\n"
             "uniform highp mat4 color_matrix;\n"
             "uniform lowp float opacity;\n"
             "void main() {\n"
             "  highp vec4 yuv = vec4(texture2D(tex0, v_texcoord * crop0).r,\n"
             "                        texture2D(tex1, v_texcoord * crop1).ra,\n"
             "                        1.0);\n"
             "  gl_FragColor = vec4((color_matrix * yuv).rgb, 1.0) * opacity;\n"
             "}\n";
    case GlVideoMode::External:
      return "#extension GL_OES_EGL_image_external : require\n"
             "#ifdef GL_ES\n"
             "precision highp float;\n"
             "#endif\n"
             "varying highp vec2 v_texcoord;\n"
             "uniform samplerExternalOES tex0;\n"
             "uniform highp vec2 crop0;\n"
             "uniform lowp float opacity;\n"
             "void main() {\n"
             "  gl_FragColor =\n"
             "      vec4(texture2D(tex0, v_texcoord * crop0).rgb, 1.0) * "
             "opacity;\n"
             "}\n";
    }
    return nullptr;
  }

  void initialize() override {
    static const char *const tex_names[3] = {"tex0", "tex1", "tex2"};
    static const char *const crop_names[3] = {"crop0", "crop1", "crop2"};
    QOpenGLShaderProgram *p = program();
    matrix_loc_ = p->uniformLocation("qt_Matrix");
    opacity_loc_ = p->uniformLocation("opacity");
    color_loc_ = p->uniformLocation("color_matrix");
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
  int matrix_loc_ = -1;
  int opacity_loc_ = -1;
  int color_loc_ = -1;
  int tex_loc_[3] = {-1, -1, -1};
  int crop_loc_[3] = {-1, -1, -1};
};

inline QSGMaterialShader *GlVideoMaterial::createShader() const {
  return new GlVideoShader(mode_);
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

  void SetRect(const QRectF &rect) {
    if (rect == rect_)
      return;
    rect_ = rect;
    QSGGeometry::updateTexturedRectGeometry(&geometry_, rect,
                                            QRectF(0, 0, 1, 1));
    markDirty(QSGNode::DirtyGeometry);
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
    if (!material_ || material_->mode() != mode) {
      material_ = new GlVideoMaterial(mode);
      setMaterial(material_); // OwnsMaterial：旧材质自动删除
    }
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
  QRectF rect_;
  GlVideoMaterial *material_ = nullptr;
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

  // 接管 frame 所有权
  void SetFrame(AVFrame *frame, const QRectF &rect, int rotation, qreal dpr) {
#if FFPLAYER_GL_RENDER
    if (gl_node_) {
      if (gl_node_->SetFrame(frame))
        has_content_ = true;
      return;
    }
#endif
    // CPU 路径：直接转换到显示尺寸（不放大）
    const QRectF tex_rect = ComputePlacement(rect, rotation).tex_rect;
    const int dst_w = std::clamp(
        static_cast<int>(std::lround(tex_rect.width() * dpr)), 1, frame->width);
    const int dst_h =
        std::clamp(static_cast<int>(std::lround(tex_rect.height() * dpr)), 1,
                   frame->height);
    if (converter_.ToRgba(frame, dst_w, dst_h, image_)) {
      if (QSGTexture *texture = window_->createTextureFromImage(image_)) {
        tex_node_->setTexture(texture);
        has_content_ = true;
      }
    }
    av_frame_free(&frame);
  }

  bool HasContent() const { return has_content_; }

  void UpdateLayout(const QRectF &rect, int rotation) {
    const Placement p = ComputePlacement(rect, rotation);
#if FFPLAYER_GL_RENDER
    if (gl_node_)
      gl_node_->SetRect(p.tex_rect);
#endif
    if (tex_node_)
      tex_node_->setRect(p.tex_rect);
    setMatrix(p.matrix);
  }

private:
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
  QSGSimpleTextureNode *tex_node_ = nullptr;
  SwsConverter converter_;
  QImage image_;
  bool has_content_ = false;
};

} // namespace render
} // namespace ffmpeg_player

#endif
