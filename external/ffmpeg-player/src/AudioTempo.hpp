#pragma once

#include "FFmpegCore.hpp"
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

namespace ffmpeg_player {

// atempo 保持音调，aformat 同时完成设备采样率/声道/S16 转换。
class AudioTempo {
  AVFilterGraph *graph_ = nullptr;
  AVFilterContext *source_ = nullptr;
  AVFilterContext *tempo_ = nullptr;
  AVFilterContext *sink_ = nullptr;
  double rate_ = 1;
  double output_source_end_ = NAN;

public:
  AudioTempo() = default;
  AudioTempo(const AudioTempo &) = delete;
  AudioTempo &operator=(const AudioTempo &) = delete;
  ~AudioTempo() { Reset(); }
  void Reset() {
    avfilter_graph_free(&graph_);
    source_ = tempo_ = sink_ = nullptr;
    output_source_end_ = NAN;
  }

  bool Init(const AVFrame *frame, int sample_rate, int channels, double rate) {
    Reset();
    FFmpegCore::Init();
    graph_ = avfilter_graph_alloc();
    if (!graph_)
      return false;
    graph_->nb_threads = 1;
    const int64_t layout = frame->channel_layout ? frame->channel_layout
                              : av_get_default_channel_layout(frame->channels);
    const char *format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format));
    if (!format || frame->sample_rate <= 0 || layout <= 0)
      return false;
    std::ostringstream input;
    input << "time_base=1/" << frame->sample_rate << ":sample_rate=" << frame->sample_rate
          << ":sample_fmt=" << format << ":channel_layout=" << layout;
    AVFilterContext *convert = nullptr;
    rate_ = rate;
    const std::string tempo_args = "tempo=" + std::to_string(rate) + ":source_timestamps=1";
    const std::string output = "sample_fmts=s16:sample_rates=" + std::to_string(sample_rate)
        + ":channel_layouts=" + std::to_string(av_get_default_channel_layout(channels));
    if (avfilter_graph_create_filter(&source_, avfilter_get_by_name("abuffer"), "input",
                                    input.str().c_str(), nullptr, graph_) < 0 ||
        avfilter_graph_create_filter(&tempo_, avfilter_get_by_name("atempo"), "tempo",
                                    tempo_args.c_str(), nullptr, graph_) < 0 ||
        avfilter_graph_create_filter(&convert, avfilter_get_by_name("aformat"), "format",
                                    output.c_str(), nullptr, graph_) < 0 ||
        avfilter_graph_create_filter(&sink_, avfilter_get_by_name("abuffersink"), "output",
                                    nullptr, nullptr, graph_) < 0)
      return false;
    return avfilter_link(source_, 0, tempo_, 0) >= 0 &&
           avfilter_link(tempo_, 0, convert, 0) >= 0 &&
           avfilter_link(convert, 0, sink_, 0) >= 0 && avfilter_graph_config(graph_, nullptr) >= 0;
  }

  // 与 Process 在同一音频线程调用，保留 atempo 的重叠窗口和已缓存的样本。
  int SetRate(double rate) {
    if (!tempo_ || !std::isfinite(rate) || rate < 0.5 || rate > 2.0)
      return AVERROR(EINVAL);
    const int ret = avfilter_process_command(tempo_, "tempo", std::to_string(rate).c_str(),
                                            nullptr, 0, 0);
    if (ret >= 0) rate_ = rate;
    return ret;
  }

  // Valid inside the consume callback. The filter owns the tempo-change origin.
  double OutputSourceEnd() const { return output_source_end_; }

  int Process(AVFrame *frame, const std::function<bool(AVFrame *)> &consume) {
    if (!source_ || !sink_)
      return AVERROR(EINVAL);
    int ret = av_buffersrc_add_frame_flags(source_, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0)
      return ret;
    AVFrame *out = av_frame_alloc();
    if (!out)
      return AVERROR(ENOMEM);
    while ((ret = av_buffersink_get_frame(sink_, out)) >= 0) {
      const auto *position = av_dict_get(out->metadata, "lavfi.atempo.source_end_us", nullptr, 0);
      if (position) {
        output_source_end_ = std::strtoll(position->value, nullptr, 10) / static_cast<double>(AV_TIME_BASE);
      } else if (std::isfinite(output_source_end_)) {
        // The resampler's final fractional-sample drain has no input props.
        output_source_end_ += static_cast<double>(out->nb_samples) / out->sample_rate * rate_;
      } else {
        ret = AVERROR_INVALIDDATA;
        break;
      }
      const bool keep = consume(out);
      av_frame_unref(out);
      if (!keep)
        break;
    }
    av_frame_free(&out);
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
  }
};
} // namespace ffmpeg_player
