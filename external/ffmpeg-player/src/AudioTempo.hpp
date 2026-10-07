#pragma once

#include "FFmpegCore.hpp"
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
  AVFilterContext *sink_ = nullptr;

public:
  AudioTempo() = default;
  AudioTempo(const AudioTempo &) = delete;
  AudioTempo &operator=(const AudioTempo &) = delete;
  ~AudioTempo() { Reset(); }
  void Reset() {
    avfilter_graph_free(&graph_);
    source_ = sink_ = nullptr;
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
    AVFilterContext *tempo = nullptr, *convert = nullptr;
    const std::string tempo_args = "tempo=" + std::to_string(rate);
    const std::string output = "sample_fmts=s16:sample_rates=" + std::to_string(sample_rate)
        + ":channel_layouts=" + std::to_string(av_get_default_channel_layout(channels));
    if (avfilter_graph_create_filter(&source_, avfilter_get_by_name("abuffer"), "input",
                                    input.str().c_str(), nullptr, graph_) < 0 ||
        avfilter_graph_create_filter(&tempo, avfilter_get_by_name("atempo"), "tempo",
                                    tempo_args.c_str(), nullptr, graph_) < 0 ||
        avfilter_graph_create_filter(&convert, avfilter_get_by_name("aformat"), "format",
                                    output.c_str(), nullptr, graph_) < 0 ||
        avfilter_graph_create_filter(&sink_, avfilter_get_by_name("abuffersink"), "output",
                                    nullptr, nullptr, graph_) < 0)
      return false;
    return avfilter_link(source_, 0, tempo, 0) >= 0 &&
           avfilter_link(tempo, 0, convert, 0) >= 0 &&
           avfilter_link(convert, 0, sink_, 0) >= 0 && avfilter_graph_config(graph_, nullptr) >= 0;
  }

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
