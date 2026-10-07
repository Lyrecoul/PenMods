#ifndef DEMUXER_HPP
#define DEMUXER_HPP

#include "FFmpegCore.hpp"
#include <atomic>
#include <memory>
#include <chrono>
#include <functional>

namespace ffmpeg_player {

class Demuxer {
private:
  AVFormatContext *format_ctx = nullptr;
  int video_stream_index = -1;
  int audio_stream_index = -1;
  bool has_attached_picture = false; // 标记是否有附加图片（如专辑封面）
  std::unique_ptr<Demuxer> separate_audio;
  AVPacket *pending_video = nullptr;
  AVPacket *pending_audio = nullptr;
  bool video_eof = false;
  bool audio_eof = false;
  const std::atomic<bool> *running = nullptr;
  std::function<bool()> interrupt_read;
  bool reading = false;
  int64_t timeline_origin = 0;
  std::chrono::steady_clock::time_point deadline;

  void BeginOperation() { deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15); }

  static int Interrupt(void *opaque) {
    const auto *self = static_cast<Demuxer *>(opaque);
    return (self->running && !self->running->load(std::memory_order_acquire)) ||
           (self->reading && self->interrupt_read && self->interrupt_read()) ||
           std::chrono::steady_clock::now() >= self->deadline;
  }

  void ClearPending() {
    av_packet_free(&pending_video);
    av_packet_free(&pending_audio);
    video_eof = audio_eof = false;
  }

  void Normalize(AVPacket *pkt, AVRational time_base) const {
    const int64_t offset = av_rescale_q(timeline_origin, AVRational{1, AV_TIME_BASE}, time_base);
    if (pkt->pts != AV_NOPTS_VALUE)
      pkt->pts -= offset;
    if (pkt->dts != AV_NOPTS_VALUE)
      pkt->dts -= offset;
  }

public:
  Demuxer() { FFmpegCore::Init(); }

  ~Demuxer() {
    ClearPending();
    if (format_ctx) {
      avformat_close_input(&format_ctx);
    }
  }

  // 禁止拷贝，防止多次释放内存
  Demuxer(const Demuxer &) = delete;
  Demuxer &operator=(const Demuxer &) = delete;

  inline bool Open(const std::string &url, const std::string &headers = {},
                   const std::atomic<bool> *active = nullptr,
                   const std::string &audio_url = {}) {
    ClearPending();
    separate_audio.reset();
    avformat_close_input(&format_ctx);
    video_stream_index = audio_stream_index = -1;
    has_attached_picture = false;
    timeline_origin = 0;
    running = active;
    BeginOperation();
    format_ctx = avformat_alloc_context();
    if (!format_ctx)
      return false;
    format_ctx->interrupt_callback = {Interrupt, this};
    AVDictionary *options = nullptr;
    if (!headers.empty())
      av_dict_set(&options, "headers", headers.c_str(), 0);
    av_dict_set(&options, "rw_timeout", "15000000", 0);
    int ret = avformat_open_input(&format_ctx, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
      LOG(ERROR) << "Could not open source: " << FFmpegCore::ErrorToString(ret)
                 << std::endl;
      return false;
    }

    ret = avformat_find_stream_info(format_ctx, nullptr);
    if (ret < 0) {
      LOG(ERROR) << "Could not find stream information" << std::endl;
      return false;
    }

    timeline_origin = format_ctx->start_time == AV_NOPTS_VALUE ? 0 : format_ctx->start_time;
    // 查找音视频流索引
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
      if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
          video_stream_index < 0) {
        // 检查是否是附加图片（如专辑封面）
        if (format_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
          has_attached_picture = true;
          LOG(INFO) << "Stream " << i << " is an attached picture (Album Art)";
        } else {
          video_stream_index = i;
        }
      } else if (format_ctx->streams[i]->codecpar->codec_type ==
                     AVMEDIA_TYPE_AUDIO &&
                 audio_stream_index < 0) {
        audio_stream_index = i;
      }
    }

    if (!audio_url.empty()) {
      separate_audio = std::make_unique<Demuxer>();
      if (!separate_audio->Open(audio_url, headers, active) ||
          separate_audio->GetAudioStreamIndex() < 0 || video_stream_index < 0)
        return false;
    }
    return true;
  }

  void SetReadInterrupt(std::function<bool()> callback) {
    interrupt_read = std::move(callback);
    if (separate_audio)
      separate_audio->SetReadInterrupt(interrupt_read);
  }

  inline AVCodecParameters *GetVideoCodecParameters() const {
    return (video_stream_index >= 0)
               ? format_ctx->streams[video_stream_index]->codecpar
               : nullptr;
  }

  inline AVCodecParameters *GetAudioCodecParameters() const {
    if (separate_audio)
      return separate_audio->GetAudioCodecParameters();
    return (audio_stream_index >= 0)
               ? format_ctx->streams[audio_stream_index]->codecpar
               : nullptr;
  }

  inline int GetVideoStreamIndex() const { return video_stream_index; }
  // 独立音轨使用虚拟索引，避免与视频输入中的索引冲突。
  inline int GetAudioStreamIndex() const {
    return separate_audio ? static_cast<int>(format_ctx->nb_streams)
                          : audio_stream_index;
  }

  AVRational GetAudioTimeBase() const {
    return separate_audio ? separate_audio->GetAudioTimeBase()
                          : format_ctx->streams[audio_stream_index]->time_base;
  }

  int Seek(int64_t timestamp) {
    BeginOperation();
    ClearPending();
    if (format_ctx->pb) {
      format_ctx->pb->error = 0;
      format_ctx->pb->eof_reached = 0;
    }
    const int video_result = av_seek_frame(format_ctx, -1, timestamp + timeline_origin, AVSEEK_FLAG_BACKWARD);
    // 双输入共用主输入的媒体时间原点，保留音视频之间的真实偏移。
    const int audio_result = separate_audio
        ? separate_audio->Seek(timestamp + timeline_origin - separate_audio->timeline_origin) : 0;
    return video_result < 0 ? video_result : audio_result;
  }

  // 检查是否有附加图片（如专辑封面）
  inline bool HasAttachedPicture() const { return has_attached_picture; }

  // 读取一个数据包
  inline int ReadPacket(AVPacket *pkt) {
    BeginOperation();
    reading = true;
    struct ReadGuard {
      Demuxer *self;
      ~ReadGuard() {
        self->reading = false;
        if (self->separate_audio)
          self->separate_audio->reading = false;
      }
    } guard{this};
    if (!separate_audio) {
      const int ret = av_read_frame(format_ctx, pkt);
      if (ret >= 0)
        Normalize(pkt, format_ctx->streams[pkt->stream_index]->time_base);
      return ret;
    }
    separate_audio->BeginOperation();
    separate_audio->reading = true;

    // 每个输入只预读一个包，按解码时间归并，保持有限内存和音视频公平读取。
    auto fill = [](AVFormatContext *ctx, int index, AVPacket *&pending,
                   bool &eof) -> int {
      if (pending || eof)
        return 0;
      pending = av_packet_alloc();
      if (!pending)
        return AVERROR(ENOMEM);
      for (;;) {
        const int ret = av_read_frame(ctx, pending);
        if (ret < 0) {
          av_packet_free(&pending);
          if (ret == AVERROR_EOF) {
            eof = true;
            return 0;
          }
          return ret;
        }
        if (pending->stream_index == index)
          return 0;
        av_packet_unref(pending);
      }
    };
    int ret = fill(format_ctx, video_stream_index, pending_video, video_eof);
    if (ret < 0)
      return ret;
    ret = fill(separate_audio->format_ctx, separate_audio->audio_stream_index,
               pending_audio, audio_eof);
    if (ret < 0)
      return ret;
    if (!pending_video && !pending_audio)
      return AVERROR_EOF;
    bool choose_audio = !pending_video;
    if (pending_video && pending_audio) {
      const auto timestamp = [](AVPacket *p) {
        return p->dts != AV_NOPTS_VALUE ? p->dts
             : p->pts != AV_NOPTS_VALUE ? p->pts : int64_t{0};
      };
      choose_audio = av_compare_ts(timestamp(pending_audio), GetAudioTimeBase(),
                                  timestamp(pending_video),
                                  format_ctx->streams[video_stream_index]->time_base) <= 0;
    }
    AVPacket *&selected = choose_audio ? pending_audio : pending_video;
    av_packet_move_ref(pkt, selected);
    av_packet_free(&selected);
    Normalize(pkt, choose_audio ? GetAudioTimeBase() : format_ctx->streams[video_stream_index]->time_base);
    if (choose_audio)
      pkt->stream_index = GetAudioStreamIndex();
    return 0;
  }

  // 获取 AVFormatContext 指针，以备高级用法
  inline AVFormatContext *GetRawContext() { return format_ctx; }
};

} // namespace ffmpeg_player

#endif
