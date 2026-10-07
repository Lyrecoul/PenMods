#ifndef DECODER_HPP
#define DECODER_HPP

// ============================================================
// Decoder — 带 rkmpp 硬解与智能回退的解码器封装
//
// 视频流选择顺序：
//   1. <codec>_rkmpp（RK3326 VPU：vdpu2 负责 H.264/VP8，另有 1080p HEVC 解码器；
//      不支持 VP9 与 10bit），输出 DRM_PRIME/NV12 零拷贝帧
//      - 预检：分辨率、像素格式（8bit 4:2:0）、profile
//      - avcodec_open2 失败（无 /dev/mpp_service、库缺失等）→ 软解
//   2. 运行期健康监测，出现以下情况标记硬解失败，由调用方执行回退：
//      - 连续 kMaxPacketsWithoutFrame 个包没有产出任何帧
//      - 连续 kMaxConsecutiveErrors 次 send/receive 错误
//      - 输入队列卡死超过 kStallTimeout
//      - 输出帧不是可渲染的 NV12 布局（如 10bit）
//
// 环境变量 FFPLAYER_HWDEC：
//   0/off/no  禁用硬解；force  跳过能力预检强制尝试硬解
// ============================================================

#include "DrmPrime.hpp"
#include "FFmpegCore.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

namespace ffmpeg_player {

// 解码出一帧时回调；返回 false 表示调用方不再需要后续帧（如发生 seek）
// 回调返回后帧会被 unref，需要保留请自行 av_frame_ref
using FrameCallback = std::function<bool(AVFrame *)>;

class Decoder {
public:
  Decoder() = default;
  ~Decoder() { Cleanup(); }

  Decoder(const Decoder &) = delete;
  Decoder &operator=(const Decoder &) = delete;

  bool Init(const AVCodecParameters *params, bool try_hw) {
    Cleanup();

    params_ = avcodec_parameters_alloc();
    if (!params_ || avcodec_parameters_copy(params_, params) < 0)
      return false;

    if (try_hw) {
      if (AVCodec *hw = FindHwDecoder(params_)) {
        if (Open(hw, true)) {
          LOG(INFO) << "Using hardware decoder: " << hw->name;
          return true;
        }
        LOG(WARNING) << hw->name << " open failed, falling back to software";
      }
    }

    AVCodec *sw = FindSwDecoder(params_->codec_id);
    if (!sw) {
      LOG(ERROR) << "Decoder not found for codec: "
                 << avcodec_get_name(params_->codec_id);
      return false;
    }
    if (!Open(sw, false))
      return false;
    LOG(INFO) << "Using software decoder: " << sw->name;
    return true;
  }

  // 送入一个包并取出所有可用帧；pkt 为 nullptr 表示 drain（EOF 冲刷）。
  // 返回 AVERROR_EOF 表示解码器已完全排空，其余情况返回 0 或错误码。
  int Decode(const AVPacket *pkt, const FrameCallback &on_frame) {
    if (!ctx_)
      return AVERROR(EINVAL);
    if (!frame_ && !(frame_ = av_frame_alloc()))
      return AVERROR(ENOMEM);

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      int ret = avcodec_send_packet(ctx_, pkt);
      if (ret == 0 || ret == AVERROR_EOF)
        break;
      if (ret != AVERROR(EAGAIN))
        return OnError(ret);

      // 输入队列已满（rkmpp 常见）：先取帧腾出空间再重发，不能丢包
      int got = 0;
      ret = ReceiveAll(on_frame, got);
      if (ret == kStopped)
        return 0;
      if (ret == AVERROR_EOF)
        return ret;
      if (ret < 0 && ret != AVERROR(EAGAIN))
        return OnError(ret);
      if (got == 0) {
        if (std::chrono::steady_clock::now() - start > kStallTimeout) {
          MarkHwFailed("decoder input stalled");
          return AVERROR(EIO);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    if (pkt && hw_ && ++packets_since_frame_ > kMaxPacketsWithoutFrame) {
      MarkHwFailed("no output frames");
      return AVERROR(EIO);
    }

    for (;;) {
      int got = 0;
      int ret = ReceiveAll(on_frame, got);
      if (ret == kStopped)
        return 0;
      if (ret == AVERROR_EOF)
        return ret;
      if (ret == AVERROR(EAGAIN)) {
        // drain 时 rkmpp 可能因阻塞超时返回 EAGAIN，需要继续取直到 EOF
        if (!pkt && std::chrono::steady_clock::now() - start < kStallTimeout)
          continue;
        return 0;
      }
      return OnError(ret);
    }
  }

  void Flush() {
    if (ctx_)
      avcodec_flush_buffers(ctx_);
    packets_since_frame_ = 0;
    consecutive_errors_ = 0;
  }

  bool IsHardware() const { return hw_; }
  bool NeedsFallback() const { return hw_ && hw_failed_; }
  const char *Name() const { return codec_ ? codec_->name : "none"; }
  AVCodecContext *GetContext() { return ctx_; }

  // 硬解失败时切换到软解（同一码流参数重新打开）
  bool FallbackToSoftware() {
    if (!hw_ || !params_)
      return false;
    AVCodec *sw = FindSwDecoder(params_->codec_id);
    if (!sw || !Open(sw, false)) {
      LOG(ERROR) << "Software fallback failed";
      return false;
    }
    LOG(WARNING) << "Switched to software decoder: " << sw->name;
    return true;
  }

  // 软解追帧策略：0 正常；1 非参考帧跳过环路滤波；2 全部跳过环路滤波并丢弃非参考帧
  int GetSkipLevel() const { return skip_level_; }
  void SetSkipLevel(int level) {
    if (!ctx_ || hw_ || level == skip_level_)
      return;
    skip_level_ = level;
    ctx_->skip_loop_filter = level >= 2   ? AVDISCARD_ALL
                             : level == 1 ? AVDISCARD_NONREF
                                          : AVDISCARD_DEFAULT;
    ctx_->skip_frame = level >= 2 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    LOG(INFO) << "Software decoder skip level -> " << level;
  }

private:
  static constexpr int kStopped = 1;
  static constexpr int kMaxPacketsWithoutFrame = 64;
  // seek 或码流损坏后 MPP 会连续吐出若干 errinfo 帧，阈值不能太低
  static constexpr int kMaxConsecutiveErrors = 16;
  static constexpr auto kStallTimeout = std::chrono::seconds(2);
  // RK3326 VPU 规格上限
  static constexpr int64_t kMaxHwPixels = 1920LL * 1088;

  void Cleanup() {
    FreeContext();
    if (params_)
      avcodec_parameters_free(&params_);
    if (frame_)
      av_frame_free(&frame_);
  }

  void FreeContext() {
    if (ctx_)
      avcodec_free_context(&ctx_);
    codec_ = nullptr;
    hw_ = false;
    hw_failed_ = false;
    skip_level_ = 0;
    packets_since_frame_ = 0;
    consecutive_errors_ = 0;
  }

  bool Open(AVCodec *codec, bool hw) {
    FreeContext();

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_)
      return false;
    // parameters_to_context 已包含 extradata 的拷贝
    if (avcodec_parameters_to_context(ctx_, params_) < 0) {
      FreeContext();
      return false;
    }

    if (hw) {
      ctx_->thread_count = 1;
    } else {
      ctx_->thread_count = 0;
      ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
      if (ctx_->codec_type == AVMEDIA_TYPE_VIDEO)
        ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0) {
      LOG(ERROR) << "Failed to open codec " << codec->name << ": "
                 << FFmpegCore::ErrorToString(ret);
      FreeContext();
      return false;
    }

    codec_ = codec;
    hw_ = hw;
    return true;
  }

  int ReceiveAll(const FrameCallback &on_frame, int &got) {
    for (;;) {
      int ret = avcodec_receive_frame(ctx_, frame_);
      if (ret < 0)
        return ret;

      ++got;
      packets_since_frame_ = 0;
      consecutive_errors_ = 0;

      if (hw_ && !drm::IsNv12(frame_)) {
        av_frame_unref(frame_);
        MarkHwFailed("unsupported output frame layout");
        return AVERROR(ENOSYS);
      }

      const bool keep_going = on_frame(frame_);
      av_frame_unref(frame_);
      if (!keep_going)
        return kStopped;
    }
  }

  int OnError(int err) {
    if (hw_) {
      LOG(WARNING) << codec_->name
                   << " error: " << FFmpegCore::ErrorToString(err);
      if (++consecutive_errors_ >= kMaxConsecutiveErrors)
        MarkHwFailed("too many decode errors");
    }
    // 软解的 INVALIDDATA 等错误属于码流损坏，跳过该包继续即可
    return err;
  }

  void MarkHwFailed(const char *reason) {
    if (hw_ && !hw_failed_) {
      LOG(WARNING) << "Hardware decoder " << codec_->name
                   << " unhealthy: " << reason;
      hw_failed_ = true;
    }
  }

  static bool HwDisabledByEnv(bool &force) {
    const char *mode = std::getenv("FFPLAYER_HWDEC");
    force = mode && std::strcmp(mode, "force") == 0;
    return mode && (std::strcmp(mode, "0") == 0 ||
                    std::strcmp(mode, "off") == 0 ||
                    std::strcmp(mode, "no") == 0);
  }

  static bool IsHwSuitable(const AVCodecParameters *p, std::string &why) {
    if (p->width > 0 && p->height > 0 &&
        static_cast<int64_t>(p->width) * p->height > kMaxHwPixels) {
      why = "resolution " + std::to_string(p->width) + "x" +
            std::to_string(p->height) + " exceeds VPU limit";
      return false;
    }
    const auto fmt = static_cast<AVPixelFormat>(p->format);
    if (fmt != AV_PIX_FMT_NONE && fmt != AV_PIX_FMT_YUV420P &&
        fmt != AV_PIX_FMT_YUVJ420P && fmt != AV_PIX_FMT_NV12) {
      const char *name = av_get_pix_fmt_name(fmt);
      why = std::string("pixel format ") + (name ? name : "unknown") +
            " not supported by VPU";
      return false;
    }
    return true;
  }

  static AVCodec *FindHwDecoder(const AVCodecParameters *p) {
    bool force = false;
    if (HwDisabledByEnv(force))
      return nullptr;

    const char *name = nullptr;
    switch (p->codec_id) {
    case AV_CODEC_ID_H264:
      name = "h264_rkmpp";
      break;
    case AV_CODEC_ID_HEVC:
      name = "hevc_rkmpp";
      break;
    case AV_CODEC_ID_VP8:
      name = "vp8_rkmpp";
      break;
    default:
      return nullptr;
    }

    AVCodec *codec = avcodec_find_decoder_by_name(name);
    if (!codec) {
      LOG(INFO) << name << " not built into libavcodec, using software";
      return nullptr;
    }

    std::string why;
    if (!force && !IsHwSuitable(p, why)) {
      LOG(INFO) << "Skip " << name << ": " << why;
      return nullptr;
    }
    return codec;
  }

  static bool IsHwWrapper(const AVCodec *codec) {
    return std::strstr(codec->name, "_rkmpp") != nullptr;
  }

  // avcodec_find_decoder 按注册顺序返回，确保拿到的不是硬解封装
  static AVCodec *FindSwDecoder(AVCodecID id) {
    AVCodec *codec = avcodec_find_decoder(id);
    if (codec && !IsHwWrapper(codec))
      return codec;
    for (AVCodec *c = av_codec_next(nullptr); c; c = av_codec_next(c)) {
      if (c->id == id && av_codec_is_decoder(c) && !IsHwWrapper(c))
        return c;
    }
    return nullptr;
  }

  AVCodecParameters *params_ = nullptr;
  AVCodecContext *ctx_ = nullptr;
  AVCodec *codec_ = nullptr;
  AVFrame *frame_ = nullptr;
  bool hw_ = false;
  bool hw_failed_ = false;
  int skip_level_ = 0;
  int packets_since_frame_ = 0;
  int consecutive_errors_ = 0;
};

} // namespace ffmpeg_player

#endif
