#ifndef DEMUXER_HPP
#define DEMUXER_HPP

#include "FFmpegCore.hpp"
#include "Mp4Metadata.hpp"
#include <atomic>
#include <memory>
#include <chrono>
#include <functional>
#include <algorithm>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

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
  std::atomic<bool> read_interrupted_by_seek{false};
  int64_t timeline_origin = 0;
  std::chrono::steady_clock::time_point deadline;
  bool network = false, first_packet = true;
  std::chrono::steady_clock::time_point opened_at;
  std::string error_;
  int64_t progress_bytes = 0;
  int64_t input_bytes = 0;
  std::thread input_threads_[2];
  std::atomic<bool> stop_inputs_{false};
  std::atomic<bool> startup_prefetch_{false};
  const std::atomic<bool> *parent_stop_ = nullptr;
  std::mutex input_mutex_;
  std::condition_variable input_changed_;
  int input_errors_[2] = {0, 0};
  double delivered_[2] = {0, 0};

  void StopInputs() {
    stop_inputs_.store(true);
    input_changed_.notify_all();
    for (auto &thread : input_threads_) if (thread.joinable()) thread.join();
    stop_inputs_.store(false);
  }

  void InputLoop(bool audio) {
    auto *source = audio ? separate_audio.get() : this;
    const int index = audio ? source->audio_stream_index : video_stream_index;
    auto &pending = audio ? pending_audio : pending_video;
    auto &eof = audio ? audio_eof : video_eof;
    while (!stop_inputs_.load() && (!running || running->load())) {
      {
        std::unique_lock<std::mutex> lock(input_mutex_);
        input_changed_.wait_for(lock, std::chrono::milliseconds(10), [&] {
          return !pending || stop_inputs_.load() || (running && !running->load());
        });
        if (pending) continue;
      }
      if (stop_inputs_.load() || (running && !running->load())) break;
      AVPacket *packet = av_packet_alloc();
      int result = packet ? 0 : AVERROR(ENOMEM);
      if (packet) {
        source->BeginOperation();
        source->reading = true;
        do {
          av_packet_unref(packet);
          result = av_read_frame(source->format_ctx, packet);
        } while (result >= 0 && packet->stream_index != index);
        source->SampleNetwork();
        source->reading = false;
      }
      std::lock_guard<std::mutex> lock(input_mutex_);
      if (result >= 0) pending = packet;
      else {
        av_packet_free(&packet);
        if (result == AVERROR_EOF) eof = true;
        else if (result != AVERROR(EAGAIN)) input_errors_[audio ? 1 : 0] = result;
      }
      input_changed_.notify_all();
      if (result < 0 && result != AVERROR(EAGAIN)) break;
    }
  }

  int ReadSeparate(AVPacket *packet) {
    if (!input_threads_[0].joinable()) {
      separate_audio->parent_stop_ = &stop_inputs_;
      input_threads_[0] = std::thread([this] { InputLoop(false); });
      input_threads_[1] = std::thread([this] { InputLoop(true); });
    }
    std::unique_lock<std::mutex> lock(input_mutex_);
    for (int error : input_errors_) if (error) return error;
    if (video_eof && audio_eof && !pending_video && !pending_audio) return AVERROR_EOF;
    bool audio = !pending_video;
    const auto time = [&](const AVPacket *p, bool isAudio) {
      const auto tb = isAudio ? GetAudioTimeBase() : format_ctx->streams[video_stream_index]->time_base;
      const auto pts = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
      return pts == AV_NOPTS_VALUE ? delivered_[isAudio] : pts * av_q2d(tb) - timeline_origin / double(AV_TIME_BASE);
    };
    if (pending_audio && pending_video) audio = time(pending_audio, true) <= time(pending_video, false);
    AVPacket *&selected = audio ? pending_audio : pending_video;
    // During startup a blocked shorter track cannot let the other fill the
    // entire cache. After startup both inputs still retain only one tail packet.
    const bool otherEof = audio ? video_eof : audio_eof;
    if (!selected || (!otherEof && startup_prefetch_.load(std::memory_order_acquire) &&
        time(selected, audio) > delivered_[!audio] + 2)) {
      input_changed_.wait_for(lock, std::chrono::milliseconds(10));
      return AVERROR(EAGAIN);
    }
    delivered_[audio] = time(selected, audio);
    av_packet_move_ref(packet, selected);
    av_packet_free(&selected);
    Normalize(packet, audio ? GetAudioTimeBase() : format_ctx->streams[video_stream_index]->time_base);
    if (audio) packet->stream_index = GetAudioStreamIndex();
    input_changed_.notify_all();
    return 0;
  }

  using NetworkCounter = std::shared_ptr<std::atomic<int64_t>>;
  NetworkCounter network_bytes;
  bool owns_network_counter;
  struct TrackedInput { AVIOContext *io; int64_t bytes; };
  std::vector<TrackedInput> network_inputs;
  decltype(AVFormatContext::io_open) original_io_open = nullptr;
  decltype(AVFormatContext::io_close) original_io_close = nullptr;

  // 仅在操作 FFmpeg 的线程读取 AVIO 字段，GUI 只读取原子计数。
  void SampleNetwork() {
    for (auto &input : network_inputs) {
      const int64_t bytes = input.io->network_bytes_read;
      if (bytes > input.bytes) {
        const int64_t delta = bytes - input.bytes;
        input_bytes += delta;
        network_bytes->fetch_add(delta, std::memory_order_relaxed);
      }
      input.bytes = bytes;
    }
  }

  void TrackNetworkInput(AVIOContext *io) {
    if (!io) return;
    const auto existing = std::find_if(network_inputs.begin(), network_inputs.end(),
                                       [io](const TrackedInput &input) { return input.io == io; });
    if (existing == network_inputs.end())
      network_inputs.push_back({io, 0});
  }

  static bool IsHttpInput(const char *url) {
    if (!url) return false;
    if (std::strncmp(url, "crypto+", 7) == 0 || std::strncmp(url, "crypto:", 7) == 0)
      url += 7;
    return std::strncmp(url, "http://", 7) == 0 || std::strncmp(url, "https://", 8) == 0;
  }

  void ResetIOState() {
    if (!format_ctx || !format_ctx->pb) return;
    auto *io = format_ctx->pb;
    io->error = 0;
    io->eof_reached = 0;
    io->buf_ptr = io->buf_end = io->buffer;
    io->buf_ptr_max = io->buffer;
  }

  static int OpenInput(AVFormatContext *ctx, AVIOContext **io, const char *url,
                       int flags, AVDictionary **options) {
    auto *self = static_cast<Demuxer *>(ctx->opaque);
    const int result = self->original_io_open(ctx, io, url, flags, options);
    // 包含主输入以及 DASH 通过 io_open 打开的分片；本地文件不计入网速。
    if (result >= 0 && IsHttpInput(url)) {
      self->TrackNetworkInput(*io);
      self->SampleNetwork();
    }
    return result;
  }

  static void CloseInput(AVFormatContext *ctx, AVIOContext *io) {
    auto *self = static_cast<Demuxer *>(ctx->opaque);
    self->SampleNetwork();
    auto &inputs = self->network_inputs;
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                [io](const TrackedInput &input) { return input.io == io; }), inputs.end());
    self->original_io_close(ctx, io);
  }

  void BeginOperation() {
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    progress_bytes = input_bytes;
  }

  static int Interrupt(void *opaque) {
    auto *self = static_cast<Demuxer *>(opaque);
    self->SampleNetwork();
    if (self->input_bytes != self->progress_bytes) self->BeginOperation();
    const bool seekInterrupt = self->reading && self->interrupt_read && self->interrupt_read();
    if (seekInterrupt)
      self->read_interrupted_by_seek.store(true, std::memory_order_release);
    return (self->running && !self->running->load(std::memory_order_acquire)) ||
           self->stop_inputs_.load() || (self->parent_stop_ && self->parent_stop_->load()) ||
           seekInterrupt || (self->network && std::chrono::steady_clock::now() >= self->deadline);
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
  const std::string &Error() const { return error_; }
  void Trace(const char *stage) const {
    const auto *io = format_ctx ? format_ctx->pb : nullptr;
    LOG(INFO) << "STARTUP stage=" << stage << " ms="
        << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - opened_at).count()
        << " bytes=" << (io ? io->bytes_read : 0) << " seeks=" << (io ? io->seek_count : 0);
  }
  explicit Demuxer(NetworkCounter counter = {})
      : network_bytes(counter ? counter : std::make_shared<std::atomic<int64_t>>(0)),
        owns_network_counter(!counter) { FFmpegCore::Init(); }

  ~Demuxer() {
    StopInputs();
    ClearPending();
    if (format_ctx) {
      SampleNetwork();
      network_inputs.clear();
      avformat_close_input(&format_ctx);
    }
  }

  // 禁止拷贝，防止多次释放内存
  Demuxer(const Demuxer &) = delete;
  Demuxer &operator=(const Demuxer &) = delete;

  inline bool Open(const std::string &url, const std::string &headers = {},
                   const std::atomic<bool> *active = nullptr,
                   const std::string &audio_url = {}) {
    StopInputs();
    ClearPending();
    input_errors_[0] = input_errors_[1] = 0;
    delivered_[0] = delivered_[1] = 0;
    separate_audio.reset();
    opened_at = std::chrono::steady_clock::now();
    error_.clear();
    first_packet = true;
    startup_prefetch_.store(false, std::memory_order_release);
    network = url.find("://") != std::string::npos && url.compare(0, 7, "file://") != 0;
    read_interrupted_by_seek.store(false, std::memory_order_release);
    SampleNetwork();
    network_inputs.clear();
    avformat_close_input(&format_ctx);
    if (owns_network_counter) network_bytes->store(0, std::memory_order_relaxed);
    video_stream_index = audio_stream_index = -1;
    has_attached_picture = false;
    timeline_origin = 0;
    running = active;
    BeginOperation();
    format_ctx = avformat_alloc_context();
    if (!format_ctx)
      return false;
    format_ctx->interrupt_callback = {Interrupt, this};
    format_ctx->opaque = this;
    original_io_open = format_ctx->io_open;
    original_io_close = format_ctx->io_close;
    format_ctx->io_open = OpenInput;
    format_ctx->io_close = CloseInput;
    AVDictionary *options = nullptr;
    if (!headers.empty())
      av_dict_set(&options, "headers", headers.c_str(), 0);
    av_dict_set(&options, "rw_timeout", "15000000", 0);
    int ret = avformat_open_input(&format_ctx, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
      error_ = "open input: " + FFmpegCore::ErrorToString(ret);
      // avformat_open_input 已释放失败的主输入。
      network_inputs.clear();
      LOG(ERROR) << "Could not open source: " << FFmpegCore::ErrorToString(ret)
                 << std::endl;
      return false;
    }

    // avformat_open_input creates the root AVIOContext directly; io_open only
    // observes nested HLS/DASH inputs and independent fragments.
    if (IsHttpInput(url.c_str()))
      TrackNetworkInput(format_ctx->pb);

    Trace("open-input");
    bool complete = !network && format_ctx->pb && format_ctx->pb->seekable &&
        std::strstr(format_ctx->iformat->name, "mov") && OrdinaryMp4(url.c_str());
    bool videoChecked = false, audioChecked = false;
    for (unsigned i = 0; complete && i < format_ctx->nb_streams; ++i) {
      const auto *st = format_ctx->streams[i];
      const auto *p = st->codecpar;
      const bool video = p->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC);
      const bool audio = p->codec_type == AVMEDIA_TYPE_AUDIO;
      if ((!video || videoChecked) && (!audio || audioChecked)) continue;
      if (video) videoChecked = true;
      if (audio) audioChecked = true;
      complete = st->time_base.num > 0 && st->time_base.den > 0 && st->nb_index_entries > 0 &&
          p->codec_id != AV_CODEC_ID_NONE && p->extradata_size > 0 &&
          (video ? p->width > 0 && p->height > 0 : p->sample_rate > 0 && p->channels > 0);
    }
    complete = complete && (videoChecked || audioChecked);
    BeginOperation();
    ret = complete ? 0 : avformat_find_stream_info(format_ctx, nullptr);
    Trace(complete ? "metadata-fast-path" : "stream-probe");
    SampleNetwork();
    if (ret < 0) {
      error_ = "stream probe: " + FFmpegCore::ErrorToString(ret);
      LOG(ERROR) << error_;
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
      separate_audio = std::make_unique<Demuxer>(network_bytes);
      if (!separate_audio->Open(audio_url, headers, active) ||
          separate_audio->GetAudioStreamIndex() < 0 || video_stream_index < 0) {
        error_ = "independent audio: " + separate_audio->Error();
        return false;
      }
    }
    return true;
  }

  int64_t NetworkBytes() const { return network_bytes->load(std::memory_order_relaxed); }

  void SetStartupPrefetch(bool active) {
    startup_prefetch_.store(active, std::memory_order_release);
    input_changed_.notify_all();
  }

  bool ConsumeSeekInterruptedRead() {
    bool interrupted = read_interrupted_by_seek.exchange(false, std::memory_order_acq_rel);
    if (separate_audio)
      interrupted = separate_audio->ConsumeSeekInterruptedRead() || interrupted;
    return interrupted;
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
    StopInputs();
    input_errors_[0] = input_errors_[1] = 0;
    delivered_[0] = delivered_[1] = timestamp / double(AV_TIME_BASE);
    BeginOperation();
    ClearPending();
    ResetIOState();
    const int video_result = av_seek_frame(format_ctx, -1, timestamp + timeline_origin, AVSEEK_FLAG_BACKWARD);
    // 双输入共用主输入的媒体时间原点，保留音视频之间的真实偏移。
    const int audio_result = separate_audio
        ? separate_audio->Seek(timestamp + timeline_origin - separate_audio->timeline_origin) : 0;
    SampleNetwork();
    return video_result < 0 ? video_result : audio_result;
  }

  // 检查是否有附加图片（如专辑封面）
  inline bool HasAttachedPicture() const { return has_attached_picture; }

  // 读取一个数据包
  inline int ReadPacket(AVPacket *pkt) {
    if (separate_audio) return ReadSeparate(pkt);
    BeginOperation();
    reading = true;
    const int ret = av_read_frame(format_ctx, pkt);
    SampleNetwork();
    reading = false;
    if (ret >= 0) {
      if (first_packet) { Trace("first-packet"); first_packet = false; }
      Normalize(pkt, format_ctx->streams[pkt->stream_index]->time_base);
    }
    return ret;
  }
  // 获取 AVFormatContext 指针，以备高级用法
  inline AVFormatContext *GetRawContext() { return format_ctx; }
};

} // namespace ffmpeg_player

#endif
