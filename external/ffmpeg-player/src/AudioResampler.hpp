#ifndef AUDIO_RESAMPLER_HPP
#define AUDIO_RESAMPLER_HPP

#include "FFmpegCore.hpp"
#include <cstdint>

namespace ffmpeg_player {

class AudioResampler {
private:
  SwrContext *swr_ctx = nullptr;

  // 目标参数（可以根据需求修改）
  AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
  int out_sample_rate = 44100;
  int64_t out_ch_layout = AV_CH_LAYOUT_STEREO;
  int out_channels = 2;

  // 输入参数（用于调试和日志）
  AVSampleFormat in_sample_fmt = AV_SAMPLE_FMT_NONE;
  int in_sample_rate = 0;
  int64_t in_ch_layout = 0;
  int in_channels = 0;

public:
  AudioResampler() {
    LOG(INFO) << "AudioResampler created with default output: "
              << out_sample_rate << "Hz, " << out_channels
              << " channels, format: S16";
  }

  // 带参数的构造函数，允许自定义输出格式
  AudioResampler(int64_t output_ch_layout, AVSampleFormat output_sample_fmt,
                 int output_sample_rate)
      : out_sample_fmt(output_sample_fmt), out_sample_rate(output_sample_rate),
        out_ch_layout(output_ch_layout) {

    out_channels = av_get_channel_layout_nb_channels(out_ch_layout);

    LOG(INFO) << "AudioResampler created with custom output: "
              << out_sample_rate << "Hz, " << out_channels << " channels";
  }

  ~AudioResampler() {
    if (swr_ctx) {
      swr_free(&swr_ctx);
      swr_ctx = nullptr;
      LOG(INFO) << "AudioResampler destroyed";
    }
  }

  // 禁止拷贝和赋值
  AudioResampler(const AudioResampler &) = delete;
  AudioResampler &operator=(const AudioResampler &) = delete;

  // 允许移动
  AudioResampler(AudioResampler &&other) noexcept
      : swr_ctx(other.swr_ctx), out_sample_fmt(other.out_sample_fmt),
        out_sample_rate(other.out_sample_rate),
        out_ch_layout(other.out_ch_layout), out_channels(other.out_channels),
        in_sample_fmt(other.in_sample_fmt),
        in_sample_rate(other.in_sample_rate), in_ch_layout(other.in_ch_layout),
        in_channels(other.in_channels) {
    other.swr_ctx = nullptr;
  }

  // 初始化：根据解码器的参数配置重采样上下文
  inline bool Init(AVCodecParameters *params) {
    return Init(params, out_sample_rate, out_sample_fmt, out_ch_layout);
  }

  // 重载：允许指定输出参数
  inline bool Init(AVCodecParameters *params, int target_sample_rate,
                   AVSampleFormat target_sample_fmt = AV_SAMPLE_FMT_S16,
                   int64_t target_ch_layout = AV_CH_LAYOUT_STEREO) {
    // 检查参数类型是否为音频
    if (params->codec_type != AVMEDIA_TYPE_AUDIO) {
      LOG(ERROR) << "Codec parameters are not for audio";
      return false;
    }

    // 更新输出参数
    out_sample_rate = target_sample_rate;
    out_sample_fmt = target_sample_fmt;
    out_ch_layout = target_ch_layout;
    out_channels = av_get_channel_layout_nb_channels(out_ch_layout);

    return InitInternal((AVSampleFormat)params->format, params->sample_rate,
                        params->channel_layout, params->channels);
  }

  // 重载版本：接受 AVCodecContext 参数
  inline bool Init(AVCodecContext *ctx) {
    return Init(ctx, out_sample_rate, out_sample_fmt, out_ch_layout);
  }

  // 重载：允许指定输出参数
  inline bool Init(AVCodecContext *ctx, int target_sample_rate,
                   AVSampleFormat target_sample_fmt = AV_SAMPLE_FMT_S16,
                   int64_t target_ch_layout = AV_CH_LAYOUT_STEREO) {
    // 检查参数类型是否为音频
    if (ctx->codec_type != AVMEDIA_TYPE_AUDIO) {
      LOG(ERROR) << "Codec context is not for audio";
      return false;
    }

    // 更新输出参数
    out_sample_rate = target_sample_rate;
    out_sample_fmt = target_sample_fmt;
    out_ch_layout = target_ch_layout;
    out_channels = av_get_channel_layout_nb_channels(out_ch_layout);

    return InitInternal(ctx->sample_fmt, ctx->sample_rate, ctx->channel_layout,
                        ctx->channels);
  }

  // 重新初始化（当输出参数改变时调用）
  inline bool Reinit(int target_sample_rate,
                     AVSampleFormat target_sample_fmt = AV_SAMPLE_FMT_S16,
                     int64_t target_ch_layout = AV_CH_LAYOUT_STEREO) {

    if (!swr_ctx || in_sample_rate == 0) {
      LOG(ERROR)
          << "Cannot reinit: resampler not initialized or no input parameters";
      return false;
    }

    // 更新输出参数
    out_sample_rate = target_sample_rate;
    out_sample_fmt = target_sample_fmt;
    out_ch_layout = target_ch_layout;
    out_channels = av_get_channel_layout_nb_channels(out_ch_layout);

    // 释放旧的上下文
    if (swr_ctx) {
      swr_free(&swr_ctx);
      swr_ctx = nullptr;
    }

    LOG(INFO) << "Reinitializing resampler with new output: " << out_sample_rate
              << "Hz, " << out_channels << " ch";

    // 重新创建重采样上下文
    swr_ctx = swr_alloc_set_opts(nullptr,
                                 out_ch_layout,   // 输出声道布局
                                 out_sample_fmt,  // 输出采样格式
                                 out_sample_rate, // 输出采样率
                                 in_ch_layout,    // 输入声道布局
                                 in_sample_fmt,   // 输入采样格式
                                 in_sample_rate,  // 输入采样率
                                 0,               // 日志级别
                                 nullptr          // 日志上下文
    );

    if (!swr_ctx) {
      LOG(ERROR) << "Failed to allocate SwrContext";
      return false;
    }

    // 设置重采样选项为最快算法（RK3326 优化）
    av_opt_set_int(swr_ctx, "resampling_renormalize", 0, 0);
    av_opt_set_int(swr_ctx, "filter_size", 16, 0);
    av_opt_set_int(swr_ctx, "phase_shift", 10, 0);
    av_opt_set_int(swr_ctx, "linear_interp", 1, 0);

    // 初始化重采样器
    int ret = swr_init(swr_ctx);
    if (ret < 0) {
      char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
      av_strerror(ret, err_buf, sizeof(err_buf));
      LOG(ERROR) << "Failed to initialize SwrContext: " << err_buf;
      swr_free(&swr_ctx);
      return false;
    }

    LOG(INFO) << "Audio resampler reinitialized successfully";
    LOG(INFO) << "  Input:  " << in_sample_rate << "Hz, " << in_channels
              << " ch, format: " << av_get_sample_fmt_name(in_sample_fmt);
    LOG(INFO) << "  Output: " << out_sample_rate << "Hz, " << out_channels
              << " ch, format: " << av_get_sample_fmt_name(out_sample_fmt);

    return true;
  }

  // 内部初始化函数
  inline bool InitInternal(AVSampleFormat input_fmt, int input_rate,
                           int64_t input_layout, int input_ch) {
    // 释放旧的上下文
    if (swr_ctx) {
      swr_free(&swr_ctx);
      swr_ctx = nullptr;
    }

    // 保存输入参数
    in_sample_fmt = input_fmt;
    in_sample_rate = input_rate;
    in_ch_layout = input_layout;
    in_channels = input_ch;

    // 检查输入采样格式是否有效
    if (in_sample_fmt == AV_SAMPLE_FMT_NONE) {
      LOG(ERROR) << "Invalid input sample format: AV_SAMPLE_FMT_NONE";
      return false;
    }

    // 检查输入采样率是否有效
    if (in_sample_rate <= 0) {
      LOG(ERROR) << "Invalid input sample rate: " << in_sample_rate;
      return false;
    }

    // 检查输入声道数是否有效
    if (in_channels <= 0) {
      LOG(ERROR) << "Invalid input channels: " << in_channels;
      return false;
    }

    // 如果声道布局为 0，根据声道数生成默认布局
    if (in_ch_layout == 0) {
      in_ch_layout = av_get_default_channel_layout(in_channels);
      if (in_ch_layout == 0) {
        LOG(ERROR) << "Failed to get default channel layout for " << in_channels
                   << " channels";
        return false;
      }
      LOG(WARNING) << "Channel layout not set, using default: 0x" << std::hex
                   << in_ch_layout << std::dec;
    }

    // 验证输出声道布局
    if (out_ch_layout == 0) {
      LOG(ERROR) << "Invalid output channel layout";
      return false;
    }

    // 确保输出声道数正确
    out_channels = av_get_channel_layout_nb_channels(out_ch_layout);

    // 创建重采样上下文
    swr_ctx = swr_alloc_set_opts(nullptr,
                                 out_ch_layout,   // 输出声道布局
                                 out_sample_fmt,  // 输出采样格式
                                 out_sample_rate, // 输出采样率
                                 in_ch_layout,    // 输入声道布局
                                 in_sample_fmt,   // 输入采样格式
                                 in_sample_rate,  // 输入采样率
                                 0,               // 日志级别
                                 nullptr          // 日志上下文
    );

    if (!swr_ctx) {
      LOG(ERROR) << "Failed to allocate SwrContext";
      return false;
    }

    // 设置重采样选项为最快算法（RK3326 优化）
    // 使用线性插值，计算量最小
    av_opt_set_int(swr_ctx, "resampling_renormalize", 0, 0);
    av_opt_set_int(swr_ctx, "filter_size", 16, 0);  // 较小的滤波器
    av_opt_set_int(swr_ctx, "phase_shift", 10, 0);  // 默认相位偏移
    av_opt_set_int(swr_ctx, "linear_interp", 1, 0); // 启用线性插值（最快）

    // 初始化重采样器
    int ret = swr_init(swr_ctx);
    if (ret < 0) {
      char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
      av_strerror(ret, err_buf, sizeof(err_buf));
      LOG(ERROR) << "Failed to initialize SwrContext: " << err_buf;
      swr_free(&swr_ctx);
      return false;
    }

    LOG(INFO) << "Audio resampler initialized successfully";
    LOG(INFO) << "  Input:  " << in_sample_rate << "Hz, " << in_channels
              << " ch, format: " << av_get_sample_fmt_name(in_sample_fmt);
    LOG(INFO) << "  Output: " << out_sample_rate << "Hz, " << out_channels
              << " ch, format: " << av_get_sample_fmt_name(out_sample_fmt);

    return true;
  }

  // 重采样逻辑
  // 返回值：转换后的字节数，失败返回 0
  inline int Resample(AVFrame *src_frame, uint8_t *out_buffer) {
    if (!swr_ctx || !src_frame || !out_buffer) {
      LOG(ERROR) << "Invalid parameters for Resample";
      return 0;
    }

    // 检查输入帧的格式是否与初始化时一致
    if (src_frame->format != in_sample_fmt ||
        src_frame->sample_rate != in_sample_rate ||
        static_cast<int64_t>(src_frame->channel_layout) != in_ch_layout) {
      LOG(WARNING) << "Frame parameters mismatch. Frame: fmt="
                   << src_frame->format << ", rate=" << src_frame->sample_rate
                   << ", layout=0x" << std::hex << src_frame->channel_layout
                   << std::dec << ", Expected: fmt=" << in_sample_fmt
                   << ", rate=" << in_sample_rate << ", layout=0x" << std::hex
                   << in_ch_layout << std::dec;

      // 可以选择重新初始化或继续
      // 这里我们继续，但记录警告
    }

    // 计算重采样后的输出采样数（考虑延迟）
    int64_t delay_samples = swr_get_delay(swr_ctx, src_frame->sample_rate);
    int out_count =
        av_rescale_rnd(delay_samples + src_frame->nb_samples, out_sample_rate,
                       src_frame->sample_rate, AV_ROUND_UP);

    // 添加额外的缓冲空间以防止溢出
    out_count += 256; // 增加一些余量

    // 执行转换
    uint8_t *output[] = {out_buffer};
    int ret =
        swr_convert(swr_ctx, output, out_count,
                    (const uint8_t **)src_frame->data, src_frame->nb_samples);

    if (ret < 0) {
      char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
      av_strerror(ret, err_buf, sizeof(err_buf));
      LOG(ERROR) << "Audio swr_convert error: " << err_buf;
      return 0;
    }

    // 返回实际生成的字节数：采样数 * 声道数 * 每个样品的字节数
    int bytes_per_sample = av_get_bytes_per_sample(out_sample_fmt);
    int total_bytes = ret * out_channels * bytes_per_sample;

    // 调试日志（可以注释掉）
    // LOG(DEBUG) << "Resampled " << src_frame->nb_samples << " samples -> "
    //            << ret << " samples, " << total_bytes << " bytes";

    return total_bytes;
  }

  // 获取需要的输出缓冲区大小（以字节为单位）
  inline int GetOutputBufferSize(AVFrame *src_frame) const {
    if (!src_frame)
      return 0;

    int64_t delay_samples = swr_get_delay(swr_ctx, src_frame->sample_rate);
    int out_samples =
        av_rescale_rnd(delay_samples + src_frame->nb_samples, out_sample_rate,
                       src_frame->sample_rate, AV_ROUND_UP);
    out_samples += 256; // 添加余量

    return out_samples * out_channels * av_get_bytes_per_sample(out_sample_fmt);
  }

  // 刷新重采样器中剩余的数据
  inline int Flush(uint8_t *out_buffer) {
    if (!swr_ctx || !out_buffer)
      return 0;

    uint8_t *output[] = {out_buffer};
    int ret = swr_convert(swr_ctx, output, 4096, nullptr, 0);

    if (ret < 0) {
      LOG(ERROR) << "Audio swr_convert flush error";
      return 0;
    }

    return ret * out_channels * av_get_bytes_per_sample(out_sample_fmt);
  }

  // Getter 方法
  int GetOutSampleRate() const { return out_sample_rate; }
  int GetOutChannels() const { return out_channels; }
  int64_t GetOutChannelLayout() const { return out_ch_layout; }
  AVSampleFormat GetOutFormat() const { return out_sample_fmt; }

  int GetInSampleRate() const { return in_sample_rate; }
  int GetInChannels() const { return in_channels; }
  int64_t GetInChannelLayout() const { return in_ch_layout; }
  AVSampleFormat GetInFormat() const { return in_sample_fmt; }

  // 检查是否已初始化
  bool IsInitialized() const { return swr_ctx != nullptr; }

  // 重置重采样器（用于 seek 操作）
  bool Reset() {
    if (swr_ctx) {
      swr_close(swr_ctx);
      // 强制清除残留延迟
      int ret = swr_init(swr_ctx);
      if (ret < 0) {
        LOG(ERROR) << "Failed to reset SwrContext";
        return false;
      }
      LOG(INFO) << "Audio resampler reset successfully";
    }
    return true;
  }

  // 设置输出参数（需要在 Init 之前调用，或调用后重新 Init）
  void SetOutputParams(int64_t layout, int rate, AVSampleFormat fmt) {
    out_ch_layout = layout;
    out_sample_rate = rate;
    out_sample_fmt = fmt;
    out_channels = av_get_channel_layout_nb_channels(layout);

    LOG(INFO) << "Audio resampler output params updated: " << rate << "Hz, "
              << out_channels << " ch";
  }
};

} // namespace ffmpeg_player

#endif // AUDIO_RESAMPLER_HPP