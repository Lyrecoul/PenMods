#pragma once

#include "AudioDevice.hpp"
#include "AudioTempo.hpp"
#include "BufferingStats.hpp"
#include "Clock.hpp"
#include "Decoder.hpp"
#include "Demuxer.hpp"
#include "FrameQueue.hpp"
#include "PacketCache.hpp"
#include "PlaybackController.hpp"
#include "PlaybackLifecycle.hpp"
#include "ShutdownTrace.hpp"
#include "VideoPacketDrop.hpp"
#include <QString>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace ffmpeg_player {

namespace detail {

// 安全获取帧 PTS
static inline double GetFramePts(AVFrame *frame, AVRational time_base,
                                 double &synthetic_pts, double frame_duration,
                                 const Clock *audio_clock) {
  int64_t raw_pts = frame->best_effort_timestamp;
  if (raw_pts == AV_NOPTS_VALUE)
    raw_pts = frame->pts;
  if (raw_pts == AV_NOPTS_VALUE)
    raw_pts = frame->pkt_dts;

  if (raw_pts == AV_NOPTS_VALUE) {
    if (synthetic_pts < 0.0) {
      synthetic_pts = (audio_clock && audio_clock->IsStarted())
                          ? audio_clock->GetClock()
                          : 0.0;
    } else {
      synthetic_pts += frame_duration;
    }
    return synthetic_pts;
  }

  double pts = raw_pts * av_q2d(time_base);
  if (std::abs(pts) > 86400.0) {
    if (synthetic_pts < 0.0) {
      synthetic_pts = (audio_clock && audio_clock->IsStarted())
                          ? audio_clock->GetClock()
                          : 0.0;
    } else {
      synthetic_pts += frame_duration;
    }
    return synthetic_pts;
  }

  synthetic_pts = pts;
  return pts;
}

#ifdef __linux__
static inline void SetThreadAffinityMulti(std::initializer_list<int> cpus,
                                          int priority = 0) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (int c : cpus)
    CPU_SET(c, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (priority > 0) {
    struct sched_param param;
    param.sched_priority = priority;
    pthread_setschedparam(pthread_self(), SCHED_RR, &param);
  }
}
#else
static inline void SetThreadAffinityMulti(std::initializer_list<int>, int = 0) {
}
#endif

} // namespace detail

class VideoPlayer;

// Every open owns its workers, queues and clocks. Retiring one session never
// resets storage that a later open or a destroyed QML item could still use.
class PlaybackSession : public std::enable_shared_from_this<PlaybackSession> {
  friend class VideoPlayer;
public:
  struct Callbacks {
    std::function<void()> prepared, frameReady;
    std::function<void(const QString &)> error;
    std::function<void(int)> seekFinished, endOfMedia;
    std::function<void(int, int)> videoSize;
    std::function<void(const QString &, bool)> decoderChanged;
  };

  static std::shared_ptr<PlaybackSession> Create(PacketCache::Config config,
      double rate, PlaybackUiDispatcher ui, Callbacks callbacks) {
    PlaybackCleanup::Instance();
    return std::shared_ptr<PlaybackSession>(
        new PlaybackSession(config, rate, std::move(ui), std::move(callbacks)),
        [](PlaybackSession *session) {
          PlaybackCleanup::Instance().Enqueue([session] { delete session; });
        });
  }

  static void Retire(std::shared_ptr<PlaybackSession> session) {
    if (!session) return;
    ShutdownTrace trace("session-retire", session.get());
    session->ui_.Disconnect();
    session->is_running_.store(false, std::memory_order_release);
    PlaybackCleanup::Instance().Enqueue([session = std::move(session)] {
      session->finishStop();
    });
  }

private:
  PlaybackSession(PacketCache::Config config, double rate,
      PlaybackUiDispatcher ui, Callbacks callbacks)
      : ui_(std::move(ui)), callbacks_(std::move(callbacks)),
        playback_rate_(rate), active_cache_config_(config), ready_frames_(3) {
    startup_at_ = monotonicSeconds();
    sync_trace_ = qEnvironmentVariableIsSet("FFPLAYER_SYNC_TRACE");
  }

  ~PlaybackSession() { finishStop(); }

  void open(const std::string &path, const std::string &audioPath,
            const std::string &headers) {
    demuxer_ = std::make_unique<Demuxer>();
    open_thread_ = std::thread([this, path, audioPath, headers] {
      if (!is_running_.load()) return;
      if (!demuxer_->Open(path, headers, &is_running_, audioPath)) {
        postPlaybackError(QString::fromStdString(demuxer_->Error()));
        return;
      }
      if (!is_running_.load()) return;
      auto *fmt = demuxer_->GetRawContext();
      video_stream_index_ = demuxer_->GetVideoStreamIndex();
      const bool attached = video_stream_index_ < 0 && demuxer_->HasAttachedPicture();
      if (attached) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
          if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            video_stream_index_ = static_cast<int>(i);
            break;
          }
      }
      video_decoder_ = std::make_unique<Decoder>();
      audio_decoder_ = std::make_unique<Decoder>();
      has_video_ = video_stream_index_ >= 0 &&
          video_decoder_->Init(fmt->streams[video_stream_index_]->codecpar, !attached);
      if (!is_running_.load()) return;
      has_audio_ = demuxer_->GetAudioStreamIndex() >= 0 &&
          audio_decoder_->Init(demuxer_->GetAudioCodecParameters(), false);
      demuxer_->Trace("decoder-init");
      if (!is_running_.load()) return;
      if (!has_video_ && !has_audio_) {
        postPlaybackError(QStringLiteral("No decodable streams found"));
        return;
      }
      duration_ = fmt->duration > 0 ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
      if (active_cache_config_.network && duration_ <= 0) active_cache_config_.enabled = false;
      buffer_video_ = has_video_ && !attached;
      if (has_video_) {
        auto *stream = fmt->streams[video_stream_index_];
        v_time_base_ = stream->time_base;
        video_width_ = video_decoder_->GetContext()->width;
        video_height_ = video_decoder_->GetContext()->height;
        const auto rate = av_guess_frame_rate(fmt, stream, nullptr);
        if (rate.num > 0 && rate.den > 0) v_packet_duration_ = av_q2d(av_inv_q(rate));
      } else {
        video_stream_index_ = -1;
      }
      if (has_audio_) {
        a_time_base_ = demuxer_->GetAudioTimeBase();
        const auto *ctx = audio_decoder_->GetContext();
        if (ctx->sample_rate > 0) a_packet_duration_ = static_cast<double>(ctx->frame_size) / ctx->sample_rate;
      }
      demuxer_->SetReadInterrupt([this] { return input_seek_pending_.load(); });
      packet_cache_.Open(active_cache_config_, has_video_, has_audio_, buffer_video_, seek_serial_.load());
      buffer_stats_serial_.store(seek_serial_.load(), std::memory_order_release);
      buffer_stats_generation_.store(buffer_stats_.Reset(buffer_video_, has_audio_, 0),
                                     std::memory_order_release);
      resetSyncState();
      buffer_hold_.store(true);
      initial_network_buffer_pending_ = active_cache_config_.network && active_cache_config_.enabled;
      demuxer_->SetStartupPrefetch(initial_network_buffer_pending_);
      audio_clock_.SetPaused(true);
      if (is_running_.load()) ui_.Post(callbacks_.prepared);
    });
  }

  void startWorkers() {
    if (!is_running_.load()) return;
    read_thread_ = std::thread(&PlaybackSession::readThreadFunc, this);
    if (has_video_) video_thread_ = std::thread(&PlaybackSession::videoThreadFunc, this);
    if (has_audio_) audio_thread_ = std::thread(&PlaybackSession::audioThreadFunc, this);
    present_thread_ = std::thread(&PlaybackSession::presentThreadFunc, this);
  }

  void finishStop() {
    if (stop_finished_) return;
    ShutdownTrace trace("session-cleanup", this);
    is_running_.store(false, std::memory_order_release);
    packet_cache_.Abort();
    ready_frames_.Abort();
    joinAllThreads();
    trace.Step("workers-joined");
    packet_cache_.Clear();
    ready_frames_.Clear();
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      av_frame_free(&pending_frame_);
    }
    demuxer_.reset();
    audio_decoder_.reset();
    trace.Step("input-audio-freed");
    // Rendered frames retain this session. Keep the video decoder alive until
    // their last reference is returned, then destroy it on the cleanup worker.
    stop_finished_ = true;
  }

  void joinAllThreads() {
    if (open_thread_.joinable())
      open_thread_.join();
    if (read_thread_.joinable())
      read_thread_.join();
    if (video_thread_.joinable())
      video_thread_.join();
    if (audio_thread_.joinable())
      audio_thread_.join();
    if (present_thread_.joinable())
      present_thread_.join();
  }

  static double monotonicSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  double playbackRate() const { return playback_rate_.load(); }
  bool seeking() const { return seeking_.load(std::memory_order_acquire); }

  double position() const {
    if (!audio_clock_.IsStarted())
      return 0.0;
    double pos = audio_clock_.GetClock();
    return std::max(0.0, duration_ > 0 ? std::min(pos, duration_) : pos);
  }

  void resetSyncState() {
    audio_clock_.Reset();
    audio_clock_.SetRate(playbackRate());
    playback_controller_.Reset();
  }

  void recordBufferedPacket(const AVPacket *packet, bool video, int serial) {
    if (!packet || !packet->data || packet->size <= 0 ||
        serial != buffer_stats_serial_.load(std::memory_order_acquire) ||
        serial != seek_serial_.load(std::memory_order_acquire) || playback_controller_.IsSeekRequested())
      return;
    const AVRational timeBase = video ? v_time_base_ : a_time_base_;
    const int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    const double start = timestamp != AV_NOPTS_VALUE
        ? timestamp * av_q2d(timeBase) : audio_clock_.GetClock();
    const double length = packet->duration > 0 ? packet->duration * av_q2d(timeBase)
        : video ? v_packet_duration_ : a_packet_duration_;
    if (length > 0)
      buffer_stats_.Record(buffer_stats_generation_.load(std::memory_order_acquire), video,
                           start, start + length);
  }

  bool seek(double seconds) {
    seconds = std::max(0.0, duration_ > 0 ? std::min(seconds, duration_) : seconds);
    bool notify_seeking;
    {
      std::lock_guard<std::mutex> inputLock(seek_control_mutex_);
      // 与音频回调提交时钟互斥，旧 PCM 不能在跳转后覆盖目标位置。
      std::lock_guard<std::mutex> lock(clock_update_mutex_);
      notify_seeking = !seeking_.exchange(true, std::memory_order_acq_rel);
      eof_reached_.store(false, std::memory_order_release);
      eof_signaled_.store(false, std::memory_order_release);
      playback_controller_.RequestSeek(seconds);
      const auto request = playback_controller_.GetSeekRequest();
      const int nextSerial = seek_serial_.load() + 1;
      video_recovering_.store(false);
      buffer_hold_.store(true, std::memory_order_release);
      buffer_stats_serial_.store(nextSerial, std::memory_order_release);
      buffer_stats_generation_.store(buffer_stats_.Reset(buffer_video_, has_audio_, seconds),
                                     std::memory_order_release);
      audio_waiting_.store(false);
      video_waiting_.store(false);
      audio_output_serial_.store(-1);
      video_output_serial_.store(-1);
      audio_clock_.SetClock(seconds, 0.0);
      audio_clock_.SetPaused(true);
      if (!input_seek_pending_.load() && packet_cache_.Seek(seconds, nextSerial)) {
        discard_before_.store(seconds);
        seek_serial_.store(nextSerial, std::memory_order_release);
        ready_frames_.Clear();
        eof_reached_.store(packet_cache_.Get(seconds).eof);
        playback_controller_.CompleteSeek(request.first);
        LOG(INFO) << "Packet cache seek hit: " << seconds;
      } else {
        input_seek_pending_.store(true, std::memory_order_release);
      }
    }
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      av_frame_free(&pending_frame_);
    }

    return notify_seeking;
  }

  void postPlaybackError(const QString &message) {
    if (is_running_.load())
      ui_.Post([callback = callbacks_.error, message] { callback(message); });
  }

  void completeSeek(int serial) {
    if (seeking_.load() && !playback_controller_.IsSeekRequested())
      ui_.Post([callback = callbacks_.seekFinished, serial] { callback(serial); });
  }

  void notifyVideoDecoderChanged() {
    const auto name = QString::fromLatin1(video_decoder_->Name());
    const bool hardware = video_decoder_->IsHardware();
    ui_.Post([callback = callbacks_.decoderChanged, name, hardware] { callback(name, hardware); });
  }

  struct FrameLifetime {
    std::shared_ptr<PlaybackSession> session;
    AVBufferRef *previous;
    ~FrameLifetime() { av_buffer_unref(&previous); }
  };

  bool retainFrameSession(AVFrame *frame) {
    auto *lifetime = new (std::nothrow) FrameLifetime{shared_from_this(), frame->opaque_ref};
    if (!lifetime) return false;
    auto *reference = av_buffer_create(reinterpret_cast<uint8_t *>(lifetime), sizeof(*lifetime),
        [](void *, uint8_t *data) { delete reinterpret_cast<FrameLifetime *>(data); }, nullptr, 0);
    if (!reference) {
      lifetime->previous = nullptr;
      delete lifetime;
      return false;
    }
    frame->opaque_ref = reference;
    return true;
  }

  void readThreadFunc() {
    const int v_idx = video_stream_index_;
    const int a_idx = demuxer_->GetAudioStreamIndex();
    AVPacket *pending = nullptr;

    while (is_running_.load(std::memory_order_acquire)) {
      // Seek 处理
      if (playback_controller_.IsSeekRequested()) {
        std::lock_guard<std::mutex> inputLock(seek_control_mutex_);
        if (!playback_controller_.IsSeekRequested()) continue;
        const auto request = playback_controller_.GetSeekRequest();
        const double target = request.second;
        ready_frames_.Clear();
        const int serial = seek_serial_.load() + 1;
        buffer_stats_serial_.store(serial, std::memory_order_release);
        buffer_stats_generation_.store(buffer_stats_.Reset(buffer_video_, has_audio_, target),
                                       std::memory_order_release);
        const bool interrupted = demuxer_->ConsumeSeekInterruptedRead();
        const bool hit = !interrupted && packet_cache_.Seek(target, serial);
        if (!hit) {
          av_packet_free(&pending);
          packet_cache_.Reset(target, serial);
          if (interrupted)
            LOG(INFO) << "Seek interrupted demux read; rebuilding input state";
          // Older HLS demuxers reject the exact playlist endpoint (their first
          // DTS can precede the normalized PTS origin). Read the final prefix
          // and let discard_before_ retain the user's precise output target.
          const double readTarget = duration_ > 0 ? std::min(target, std::max(0.0, duration_ - 1.0)) : target;
          const int result = demuxer_->Seek(static_cast<int64_t>(readTarget * AV_TIME_BASE));
          if (result < 0) {
            postPlaybackError(QStringLiteral("Seek failed: ") + QString::fromStdString(FFmpegCore::ErrorToString(result)));
            break;
          }
        }
        LOG(INFO) << "Packet cache seek " << (hit ? "hit: " : "miss: ") << target;
        discard_before_.store(target, std::memory_order_relaxed);
        seek_serial_.store(serial, std::memory_order_release);
        ready_frames_.Clear();
        eof_reached_.store(packet_cache_.Get(target).eof, std::memory_order_release);
        eof_signaled_.store(false, std::memory_order_relaxed);

        playback_controller_.CompleteSeek(request.first);
        input_seek_pending_.store(false, std::memory_order_release);
        continue;
      }

      // Prefetch also runs while paused. Capacity waits remain interruptible.
      packet_cache_.SetPosition(audio_clock_.GetClock());
      if (!packet_cache_.WantRead()) {
        packet_cache_.Wait();
        continue;
      }

      double missing = 0;
      if (packet_cache_.BeginRefill(missing)) {
        av_packet_free(&pending);
        const int result = demuxer_->Seek(static_cast<int64_t>(std::max(0.0, missing) * AV_TIME_BASE));
        if (result < 0) {
          postPlaybackError(QStringLiteral("Cache tail refill failed: ") +
              QString::fromStdString(FFmpegCore::ErrorToString(result)));
          break;
        }
        LOG(INFO) << "Packet cache refill missing tail: " << missing;
      }

      const bool needRead = !pending;
      if (needRead) pending = av_packet_alloc();
      if (!pending) break;
      read_waiting_.store(needRead, std::memory_order_release);
      const int ret = needRead ? demuxer_->ReadPacket(pending) : 0;
      read_waiting_.store(ret == AVERROR(EAGAIN), std::memory_order_release);
      if (ret < 0) {
        av_packet_free(&pending);
        if (playback_controller_.IsSeekRequested())
          continue;

        if (ret == AVERROR_EOF) {
          eof_reached_.store(true, std::memory_order_release);

          // Each cursor sees its own drain marker after all cached packets.
          packet_cache_.End();
          continue;
        }
        if (ret == AVERROR(EAGAIN)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        if (is_running_.load(std::memory_order_acquire))
          postPlaybackError(QStringLiteral("Media read failed: ") + QString::fromStdString(FFmpegCore::ErrorToString(ret)));
        break;
      }

      // 视频包不再在这里按时间丢弃：丢掉参考帧会导致花屏直到下一个关键帧。
      // 追帧改由解码线程丢弃解码后的帧 + 软解 skip_frame 完成。
      if (playback_controller_.IsSeekRequested()) {
        // A cache hit must retain the reader's pending tail packet. Throwing it
        // away here would leave a hole when the cursor reaches the demux tail.
        continue;
      }
      const bool video = pending->stream_index == v_idx && has_video_;
      if (video || (pending->stream_index == a_idx && has_audio_)) {
        bool accepted = false;
        while (is_running_.load() && !playback_controller_.IsSeekRequested()) {
          packet_cache_.SetPosition(audio_clock_.GetClock());
          if (packet_cache_.Push(pending, video, video ? v_time_base_ : a_time_base_,
                                video ? v_packet_duration_ : a_packet_duration_)) {
            accepted = true;
            break;
          }
          if (packet_cache_.NeedsRefill()) break;
          packet_cache_.Wait();
        }
        if (!accepted) continue;
      }
      av_packet_free(&pending);
    }
    av_packet_free(&pending);
    read_waiting_.store(false, std::memory_order_release);
  }

  bool pushFrame(AVFrame *frame, int serial, uint64_t generation) {
    while (is_running_.load(std::memory_order_relaxed)) {
      if (seek_serial_.load(std::memory_order_acquire) != serial || generation != video_generation_.load() ||
          playback_controller_.IsSeekRequested())
        return false;
      if (ready_frames_.Push(frame, 10, serial, generation))
        return true;
    }
    return false;
  }

  void videoThreadFunc() {
    AVCodecContext *ctx = video_decoder_->GetContext();
    if (!ctx)
      return;

    double frame_duration = 0.0;
    if (ctx->framerate.num > 0 && ctx->framerate.den > 0) {
      frame_duration = av_q2d(av_inv_q(ctx->framerate));
    }
    if (frame_duration <= 0.0 || frame_duration > 1.0) {
      frame_duration = v_packet_duration_ > 0 ? v_packet_duration_ : 1.0 / 25.0;
    }

    double synthetic_pts = 0.0;
    double replay_until_pts = -1.0; // 回退重放时，已显示过的帧直接跳过
    double last_output_pts = -1.0;
    bool wait_keyframe = false;
    bool video_dead = false;
    int late_streak = 0;
    int ontime_streak = 0;
    double late_since = 0, last_recovery = -1, recovery_target = 0, trace_time = 0, video_lag = 0;
    uint64_t generation = video_generation_.load(), packet_drops = 0, recoveries = 0;
    uint64_t decoded_frames = 0, preroll_drops = 0, nonref_drops = 0;
    bool recover = false, finding_key = false;
    double measured_rate = playbackRate();
    int last_seek_serial = seek_serial_.load(std::memory_order_acquire);

    // 软解跟不上时逐级跳过环路滤波/非参考帧，追上后恢复
    auto adapt_skip = [&]() {
#ifdef FFPLAYER_VALIDATION
      if (qEnvironmentVariableIsSet("FFPLAYER_TEST_NO_SKIP")) return;
#endif
      if (video_decoder_->IsHardware())
        return;
      int level = video_decoder_->GetSkipLevel();
      if (late_streak >= 12)
        level = 2;
      else if (late_streak >= 3)
        level = std::max(level, 1);
      else if (ontime_streak >= 90)
        level = 0;
      video_decoder_->SetSkipLevel(level);
    };

    const auto cancelled = [&] {
      return !is_running_.load(std::memory_order_acquire) ||
             playback_controller_.IsSeekRequested() ||
             seek_serial_.load(std::memory_order_acquire) != last_seek_serial ||
             generation != video_generation_.load(std::memory_order_acquire);
    };

    const FrameCallback on_frame = [&](AVFrame *frame) -> bool {
      ++decoded_frames;
      if (decoded_frames == 1) LOG(INFO) << "STARTUP stage=first-video ms=" << (monotonicSeconds() - startup_at_) * 1000;
      if (!is_running_.load(std::memory_order_acquire) ||
          seek_serial_.load(std::memory_order_acquire) != last_seek_serial ||
          generation != video_generation_.load() || playback_controller_.IsSeekRequested())
        return false;
      if (duration_ > 0 && discard_before_.load() >= duration_) return true;

      const double pts = detail::GetFramePts(
          frame, v_time_base_, synthetic_pts, frame_duration, &audio_clock_);
      if (pts + frame_duration < discard_before_.load(std::memory_order_relaxed)) {
        ++preroll_drops;
        return true;
      }

      if (replay_until_pts >= 0.0) {
        if (pts <= replay_until_pts + 0.001)
          return true;
        replay_until_pts = -1.0;
      }

      if (audio_clock_.IsStarted() && !buffer_hold_.load() && !seeking_.load() && !playback_controller_.IsPaused()) {
        const double diff = pts - audio_clock_.GetClock();
        video_lag = std::max(0.0, -diff);
        const double realNow = monotonicSeconds();
        if (diff < -0.5 && diff > -300) {
          if (!late_since) late_since = realNow;
          if (realNow - late_since >= 0.5 && realNow - last_recovery >= 1.0) {
            recover = true;
            return false;
          }
        } else late_since = 0;
        const bool late = std::abs(diff) < 300.0 && diff < -0.10;
        if (late) {
          ++late_streak;
          ontime_streak = 0;
        } else {
          ++ontime_streak;
          late_streak = 0;
        }
        adapt_skip();
        if (late) {
          ++callback_drops_;
          return true; // 过期帧：在像素转换/上传之前就丢弃
        }
      } else {
        late_since = 0;
        video_lag = 0;
      }

      if (!pushFrame(frame, last_seek_serial, generation))
        return false;
      last_output_pts = pts;
      return true;
    };

    while (is_running_.load(std::memory_order_acquire)) {
      // 暂停时休眠（但仍响应 seek）
      if (playback_controller_.IsPaused() &&
          !seeking_.load() && !playback_controller_.IsSeekRequested()) {
        late_since = 0;
        video_lag = 0;
        video_recovering_.store(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }

      if (finding_key && last_seek_serial != seek_serial_.load()) finding_key = false;
      if (finding_key && !playback_controller_.IsSeekRequested()) {
        recovery_target = std::max(recovery_target, audio_clock_.GetClock());
        const bool found = packet_cache_.AdvanceVideo(recovery_target, last_seek_serial, &packet_drops);
        if (!found && !packet_cache_.Get(recovery_target).eof) {
          packet_cache_.Wait();
          continue;
        }
        finding_key = false;
      }
      AVPacket *pkt = nullptr;
      int packet_serial = 0;
      if (!packet_cache_.Pop(true, pkt, packet_serial, [this] {
            return !is_running_.load() || playback_controller_.IsSeekRequested();
          }))
        continue;

      // Pop 可能跨越 seek；先确认包属于当前跳转，再清空解码器。
      // 否则等待中的消费者会丢掉新位置的第一个包（通常是关键帧）。
      if (packet_serial != seek_serial_.load(std::memory_order_acquire) ||
          playback_controller_.IsSeekRequested()) {
        av_packet_free(&pkt);
        continue;
      }
      recordBufferedPacket(pkt, true, packet_serial);
      if (packet_serial != last_seek_serial) {
        last_seek_serial = packet_serial;
        video_decoder_->Flush();
        video_decoder_->SetSkipLevel(0);
        synthetic_pts = -1.0;
        replay_until_pts = -1.0;
        last_output_pts = -1.0;
        wait_keyframe = false;
        finding_key = recover = false;
        video_recovering_.store(false);
        late_since = 0;
        video_lag = 0;
        generation = video_generation_.load();
        late_streak = 0;
        ontime_streak = 0;
        video_drained_serial_.store(video_dead ? last_seek_serial : -1, std::memory_order_release);
      }

      if (video_dead) {
        av_packet_free(&pkt);
        continue;
      }

      const bool is_flush = !pkt->data && pkt->size == 0;
      if (!is_flush) {
        if (pkt->flags & AV_PKT_FLAG_KEY) {
          wait_keyframe = false;
        } else if (wait_keyframe) {
          av_packet_free(&pkt);
          continue;
        }
      }

      // Display drops save no VPU work. Skip only provably non-reference
      // access units when decoding is late, leaving all reference/state NALs.
      // MPP can have queued pictures ahead of its latest output; waiting until
      // the next packet itself is late allows that hidden backlog to grow.
      bool allow_packet_drop = true;
#ifdef FFPLAYER_VALIDATION
      allow_packet_drop = !qEnvironmentVariableIsSet("FFPLAYER_TEST_NO_PACKET_DROP");
#endif
      const double packet_lag = pkt->pts == AV_NOPTS_VALUE ? 0 :
          audio_clock_.GetClock() - (pkt->pts * av_q2d(v_time_base_) + frame_duration);
      if (measured_rate != playbackRate()) {
        measured_rate = playbackRate();
        video_decoder_->ResetPerformance();
      }
      const bool decode_overloaded = measured_rate > 1 &&
          video_decoder_->RecentFrameSeconds() * measured_rate > frame_duration * 1.05;
      if (allow_packet_drop && !is_flush && audio_clock_.IsStarted() &&
          !buffer_hold_.load() && !seeking_.load() && !playback_controller_.IsPaused() &&
          (decode_overloaded || packet_lag > 0.05 * playbackRate() || video_lag > 0.05 * playbackRate()) &&
          std::abs(packet_lag) < 300 &&
          IsNonReferenceVideoPacket(pkt, ctx)) {
        ++nonref_drops;
#ifdef FFPLAYER_VALIDATION
        if (validation_packet_drop) validation_packet_drop();
#endif
        av_packet_free(&pkt);
        continue;
      }

      int ret = video_decoder_->Decode(is_flush ? nullptr : pkt, on_frame, cancelled);
      av_packet_free(&pkt);
      if (cancelled()) continue;
      if (recover) {
        recover = false;
        // The decode call has returned: no decoder callback can survive Flush.
        // Do not hold the audio clock lock while waiting for the renderer or
        // resetting MPP: both can block for much longer than an audio period.
        bool recovering = false;
        {
          std::lock_guard<std::mutex> control(clock_update_mutex_);
          if (is_running_.load() && !playback_controller_.IsPaused() &&
              !playback_controller_.IsSeekRequested() && last_seek_serial == seek_serial_.load()) {
            generation = ++video_generation_;
            recovery_target = audio_clock_.GetClock();
            video_recovering_.store(true);
            recovering = true;
          }
        }
        if (recovering) {
          {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            av_frame_free(&pending_frame_);
#ifdef FFPLAYER_VALIDATION
            if (validation_recovery) validation_recovery(generation);
#endif
          }
          ready_frames_.Clear();
          video_decoder_->Flush();
          video_decoder_->SetSkipLevel(0);
          replay_until_pts = -1;
          synthetic_pts = -1;
          finding_key = wait_keyframe = true;
          last_recovery = monotonicSeconds();
          ++recoveries;
          LOG(INFO) << "SYNC recovery generation=" << generation << " target=" << recovery_target;
        }
        late_since = 0;
      }
      if (sync_trace_ && monotonicSeconds() - trace_time >= 0.5) {
        trace_time = monotonicSeconds();
        LOG(INFO) << "SYNC decode time=" << std::fixed << std::setprecision(6) << trace_time << " generation=" << generation
                  << " lag=" << video_lag << " decode_seconds=" << video_decoder_->DecodeSeconds()
                  << " callback_seconds=" << video_decoder_->CallbackSeconds()
                  << " callback_drops=" << callback_drops_.load() << " present_drops=" << present_drops_.load()
                  << " render_drops=" << render_drops_.load() << " preroll_drops=" << preroll_drops
                  << " decoded_frames=" << decoded_frames << " skip_level=" << video_decoder_->GetSkipLevel()
                  << " nonref_drops=" << nonref_drops << " packet_drops=" << packet_drops << " recoveries=" << recoveries;
      }

      // ─── 硬解运行期故障：切软解并重放当前 GOP ───
      if (video_decoder_->NeedsFallback()) {
        if (!video_decoder_->FallbackToSoftware()) {
          LOG(ERROR) << "Video decoding unavailable, dropping video stream";
          video_dead = true;
          video_drained_serial_.store(last_seek_serial, std::memory_order_release);
          continue;
        }
        notifyVideoDecoderChanged();
        ctx = video_decoder_->GetContext();

        ret = 0;
        replay_until_pts = last_output_pts;
        if (!packet_cache_.ReplayVideo(last_seek_serial)) {
          // Local caches may no longer retain this GOP. Wait for the next
          // video keyframe instead of seeking and discarding continuous audio.
          wait_keyframe = true;
        }
        else
          continue;
        if (is_flush)
          ret = video_decoder_->Decode(nullptr, on_frame, cancelled);
      }

      if ((ret == AVERROR_EOF || is_flush) &&
          last_seek_serial == seek_serial_.load() && !playback_controller_.IsSeekRequested())
        video_drained_serial_.store(last_seek_serial, std::memory_order_release);
    }
  }

  void audioThreadFunc() {
    detail::SetThreadAffinityMulti({2, 3}, 95);

    AVCodecContext *ctx = audio_decoder_->GetContext();
    if (!ctx)
      return;

    int source_channels = 2;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    source_channels = ctx->ch_layout.nb_channels;
#else
    source_channels = ctx->channels;
#endif

    // 嵌入式默认输出最多立体声，多声道在滤镜内下混。
    AudioDevice audio_out(48000, std::min(2, std::max(1, source_channels)),
        [this] { return playback_controller_.IsPaused() || buffer_hold_.load(); },
        [this](bool paused, double latency) {
          std::lock_guard<std::mutex> lock(clock_update_mutex_);
          if (is_running_.load() && !playback_controller_.IsSeekRequested())
            audio_clock_.ConfirmAudioPause(paused || buffer_hold_.load() || seeking_.load(), latency);
        });
    if (!audio_out.IsValid()) {
      postPlaybackError(QStringLiteral("Cannot open ALSA audio output"));
      return;
    }
    AudioTempo tempo;
    AudioSourceTimeline source_timeline;
    bool tempo_ready = false;
    bool first_audio = true;
    double output_pts = 0.0;
    int64_t source_pts = AV_NOPTS_VALUE, output_samples = 0;
    double trace_time = 0;
    double rate = playbackRate();
    int last_seek_serial = seek_serial_.load(std::memory_order_acquire);
    const auto cancelled = [&]() {
      return !is_running_.load(std::memory_order_acquire) ||
             playback_controller_.IsSeekRequested() ||
             seek_serial_.load(std::memory_order_acquire) != last_seek_serial;
    };
    const auto output = [&](AVFrame *frame) -> bool {
      if (cancelled()) return false;
      if (frame->nb_samples <= 0 || frame->sample_rate <= 0) return true;
      audio_output_serial_.store(last_seek_serial, std::memory_order_release);
      audio_waiting_.store(false, std::memory_order_release);
      while (buffer_hold_.load(std::memory_order_acquire) && !cancelled())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!has_video_ && !cancelled()) completeSeek(last_seek_serial);
      while (playback_controller_.IsPaused() && !cancelled()) {
        audio_out.Pause(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (cancelled())
        return false;
      audio_out.Pause(false);
      const int bytes = frame->nb_samples * audio_out.GetBytesPerFrame();
      if (!audio_out.Feed(frame->data[0], bytes, cancelled))
        return false;
      const double output_duration = static_cast<double>(frame->nb_samples) / frame->sample_rate;
      const double previous_pts = output_pts;
      output_pts = std::max(previous_pts, source_timeline.At(tempo.OutputSourceEnd()));
      const double clock_rate = (output_pts - previous_pts) / output_duration;
      output_samples += frame->nb_samples;
      const auto device = audio_out.GetPlaybackStats();
#ifdef FFPLAYER_VALIDATION
      if (validation_audio) validation_audio(output_pts, output_samples, frame->nb_samples, last_seek_serial);
      if (validation_audio_device) validation_audio_device(device.dropped, device.underruns, device.flushes);
#endif
      {
        std::lock_guard<std::mutex> lock(clock_update_mutex_);
        if (cancelled())
          return false;
        audio_clock_.SetAudioClock(output_pts, output_duration, clock_rate,
                                   device.latency);
      }
      if (sync_trace_ && monotonicSeconds() - trace_time >= 0.5) {
        trace_time = monotonicSeconds();
        LOG(INFO) << "SYNC audio time=" << std::fixed << std::setprecision(6) << trace_time
                  << " serial=" << last_seek_serial << " raw_pts=" << source_pts
                  << " source_seconds=" << (source_pts == AV_NOPTS_VALUE ? -1 : source_pts * av_q2d(a_time_base_))
                  << " output_samples=" << output_samples << " sample_rate=" << frame->sample_rate
                  << " output_end=" << output_pts << " rate=" << rate << " clock_rate=" << clock_rate
                  << " alsa_delay=" << device.latency << " audio_dropped=" << device.dropped
                  << " audio_underruns=" << device.underruns << " audio_flushes=" << device.flushes
                  << " clock=" << audio_clock_.GetClock();
      }
      return true;
    };

    const FrameCallback on_frame = [&](AVFrame *frame) -> bool {
      if (first_audio) {
        LOG(INFO) << "STARTUP stage=first-audio ms=" << (monotonicSeconds() - startup_at_) * 1000;
        first_audio = false;
      }
      if (cancelled())
        return false;
      if (duration_ > 0 && discard_before_.load() >= duration_) return true;
      const int64_t raw_pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                  ? frame->best_effort_timestamp : frame->pts;
      source_pts = raw_pts;
#ifdef FFPLAYER_VALIDATION
      if (validation_audio_input && raw_pts != AV_NOPTS_VALUE)
        validation_audio_input(raw_pts * av_q2d(a_time_base_),
                               static_cast<double>(frame->nb_samples) / frame->sample_rate, last_seek_serial);
#endif
      const double pts = raw_pts != AV_NOPTS_VALUE ? raw_pts * av_q2d(a_time_base_)
          : tempo_ready ? source_timeline.EndPts() : discard_before_.load();
      if (frame->sample_rate <= 0)
        return false;
      const double target = discard_before_.load();
      const int skip = static_cast<int>(std::min<double>(frame->nb_samples,
          std::max(0.0, std::ceil((target - pts) * frame->sample_rate))));
      if (skip >= frame->nb_samples)
        return true;
      AVFrame *trimmed = av_frame_clone(frame);
      if (!trimmed)
        return false;
      const auto fmt = static_cast<AVSampleFormat>(frame->format);
      const bool planar = av_sample_fmt_is_planar(fmt);
      const int planes = planar ? frame->channels : 1;
      const int offset = skip * av_get_bytes_per_sample(fmt) * (planar ? 1 : frame->channels);
      for (int i = 0; i < planes; ++i) {
        trimmed->extended_data[i] += offset;
        if (i < AV_NUM_DATA_POINTERS)
          trimmed->data[i] = trimmed->extended_data[i];
      }
      trimmed->nb_samples -= skip;
      // 滤镜以样本时间为基准，输出时钟由原始媒体 PTS 锚定。
      trimmed->pts = (raw_pts == AV_NOPTS_VALUE ? static_cast<int64_t>(std::llround(pts * frame->sample_rate))
          : av_rescale_q(raw_pts, a_time_base_, AVRational{1, frame->sample_rate})) + skip;
      const double requested_rate = playbackRate();
      if (!tempo_ready) {
        rate = requested_rate;
        tempo_ready = tempo.Init(trimmed, audio_out.GetActualSampleRate(),
                                 audio_out.GetActualChannels(), rate);
        output_pts = pts + static_cast<double>(skip) / frame->sample_rate;
        if (!tempo_ready) {
          av_frame_free(&trimmed);
          postPlaybackError(QStringLiteral("Cannot initialize audio tempo filters"));
          return false;
        }
      } else if (requested_rate != rate) {
        const int ret = tempo.SetRate(requested_rate);
        if (ret < 0) {
          av_frame_free(&trimmed);
          postPlaybackError(QStringLiteral("Cannot change audio tempo: ") +
                            QString::fromStdString(FFmpegCore::ErrorToString(ret)));
          return false;
        }
        rate = requested_rate;
      }
      source_timeline.Append(pts + static_cast<double>(skip) / frame->sample_rate,
                             static_cast<double>(trimmed->nb_samples) / frame->sample_rate);
      const int ret = tempo.Process(trimmed, output);
      av_frame_free(&trimmed);
      if (ret < 0)
        postPlaybackError(QStringLiteral("Audio tempo processing failed"));
      return ret >= 0 && !cancelled();
    };

    while (is_running_.load(std::memory_order_acquire)) {
      if (playback_controller_.IsSeekRequested()) {
        audio_out.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      const bool paused = playback_controller_.IsPaused();
      audio_out.Pause(paused);
      if (paused && !seeking_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      AVPacket *pkt = nullptr;
      int packet_serial = 0;
      if (!packet_cache_.Pop(false, pkt, packet_serial, [this] {
            return !is_running_.load() || playback_controller_.IsSeekRequested();
          })) {
        const bool empty = !eof_reached_.load() && !seeking_.load() &&
            !playback_controller_.IsSeekRequested() && audio_out.GetCurrentLatencySeconds() <= 0.005;
        audio_waiting_.store(empty, std::memory_order_release);
        if (empty) audio_output_serial_.store(-1, std::memory_order_release);
        continue;
      }
      audio_waiting_.store(false, std::memory_order_release);
      if (packet_serial != seek_serial_.load(std::memory_order_acquire) ||
          playback_controller_.IsSeekRequested()) {
        av_packet_free(&pkt);
        continue;
      }
      recordBufferedPacket(pkt, false, packet_serial);
      if (packet_serial != last_seek_serial) {
        audio_out.Flush();
        audio_decoder_->Flush();
        tempo.Reset();
        source_timeline.Clear();
        tempo_ready = false;
        output_samples = 0;
        last_seek_serial = packet_serial;
        audio_drained_serial_.store(-1);
      }
      const bool is_flush = !pkt->data && pkt->size == 0;
      audio_decoder_->Decode(is_flush ? nullptr : pkt, on_frame, cancelled);
      av_packet_free(&pkt);
      if (is_flush && !cancelled()) {
        if (tempo_ready)
          tempo.Process(nullptr, output);
        // 音频确实播完才能通知 EOF；等待可被停止和 seek 中断。
        while (!cancelled() && audio_out.GetCurrentLatencySeconds() > 0.005) {
          audio_out.Pause(playback_controller_.IsPaused());
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!cancelled())
          audio_drained_serial_.store(last_seek_serial, std::memory_order_release);
      }
    }

    audio_out.Flush();
  }

  void presentThreadFunc() {
    double present_synthetic_pts = 0.0;
    const double frame_duration = v_packet_duration_ > 0 ? v_packet_duration_ : 1.0 / 25.0;
    int last_w = 0, last_h = 0;
    double trace_time = 0;

    while (is_running_.load(std::memory_order_acquire)) {
      // 暂停时休眠（但响应 seek）
      if (playback_controller_.IsPaused() &&
          !seeking_.load() && !playback_controller_.IsSeekRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }

      AVFrame *frame = nullptr;
      int frame_serial = 0;
      uint64_t frame_generation = 0;
      if (!ready_frames_.Pop(frame, true, 20, &frame_serial, &frame_generation)) {
        video_output_serial_.store(-1, std::memory_order_release);
        video_waiting_.store(buffer_video_ && !video_recovering_.load() && !eof_reached_.load() &&
            !seeking_.load() && !playback_controller_.IsSeekRequested() &&
            video_drained_serial_.load() != seek_serial_.load() &&
            packet_cache_.Empty(true), std::memory_order_release);
        const int eof_serial = seek_serial_.load(std::memory_order_acquire);
        // 解复用到达 EOF 且解码器已排空，才算真正播放完毕
        if (eof_reached_.load(std::memory_order_acquire) &&
            (!has_video_ || video_drained_serial_.load(std::memory_order_acquire) == seek_serial_.load()) &&
            (!has_audio_ || audio_drained_serial_.load(std::memory_order_acquire) == seek_serial_.load()) &&
            !playback_controller_.IsSeekRequested() &&
            ready_frames_.Size() == 0) {

          bool notify = false;
          {
            // Serialize with the GUI seek reset. An old drain observation must
            // not mark a new cached seek as already signaled or pause its output.
            std::lock_guard<std::mutex> lock(clock_update_mutex_);
            bool expected = false;
            if (eof_serial == seek_serial_.load() && !playback_controller_.IsSeekRequested())
              notify = eof_signaled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
          }
          if (notify) {
            // 通知主线程处理 EOF（暂停而不是停止）
            ui_.Post([callback = callbacks_.endOfMedia, serial = eof_serial] {
              callback(serial);
            });
          }

          // 等待 seek 或停止
          while (is_running_.load(std::memory_order_relaxed) &&
                 seek_serial_.load(std::memory_order_acquire) == eof_serial &&
                 eof_reached_.load(std::memory_order_relaxed) &&
                 !playback_controller_.IsSeekRequested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
        continue;
      }

      double pts =
          detail::GetFramePts(frame, v_time_base_, present_synthetic_pts,
                              frame_duration, &audio_clock_);
      if (frame_generation != video_generation_.load() || frame_serial != seek_serial_.load() || playback_controller_.IsSeekRequested() ||
          pts + frame_duration < discard_before_.load()) {
        av_frame_free(&frame);
        continue;
      }
      video_waiting_.store(false, std::memory_order_release);
      video_output_serial_.store(frame_serial, std::memory_order_release);
      while (buffer_hold_.load(std::memory_order_acquire) && is_running_.load() &&
             frame_generation == video_generation_.load() &&
             frame_serial == seek_serial_.load() && !playback_controller_.IsSeekRequested())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!is_running_.load() || frame_generation != video_generation_.load() || playback_controller_.IsSeekRequested() ||
          frame_serial != seek_serial_.load()) {
        av_frame_free(&frame);
        continue;
      }

      // 正常播放要等音频时钟建立；暂停 seek 则允许先显示目标画面。
      while (has_audio_ && !audio_clock_.IsStarted() && audio_drained_serial_.load() != seek_serial_.load() &&
             frame_generation == video_generation_.load() &&
             is_running_.load() && !playback_controller_.IsSeekRequested())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

      // 纯视频文件没有音频时钟：以首帧时间启动主时钟，否则会全速播放
      if (!has_audio_ && !audio_clock_.IsStarted())
        audio_clock_.SetClock(pts, 0.0);

      // 音视频同步
      if (audio_clock_.IsStarted() && !playback_controller_.IsPaused() && !seeking_.load()) {
        double diff = pts - audio_clock_.GetClock();
        if (std::abs(diff) < 300.0) {
          if (diff < -0.15) {
            ++present_drops_;
            av_frame_free(&frame);
            continue;
          }
          if (diff > 1.0 && !has_audio_ && frame_generation == 0) {
            // 纯视频时间戳跳变：重新对齐时钟而不是长时间等待
            audio_clock_.SetClock(pts, 0.0);
            diff = 0.0;
          }
          // 恢复取得的未来关键帧也必须等待主时钟，长 GOP 可静帧数秒。
          while (diff > 0.004 && (has_audio_ || frame_generation != 0 || diff <= 1.0) &&
                 is_running_.load(std::memory_order_relaxed) &&
                 frame_generation == video_generation_.load() &&
                 frame_serial == seek_serial_.load() && !seeking_.load() &&
                 !playback_controller_.IsSeekRequested()) {
            std::this_thread::sleep_for(std::chrono::duration<double>(
                std::min((diff - 0.002) / playbackRate(), 0.010)));
            diff = pts - audio_clock_.GetClock();
          }
          if (!is_running_.load(std::memory_order_relaxed) ||
              playback_controller_.IsSeekRequested()) {
            av_frame_free(&frame);
            continue;
          }
        }
      }

      const int w = frame->width;
      const int h = frame->height;
      // pause/seek 可能在等待同步时发生，最后交帧前再次检查。
      while (playback_controller_.IsPaused() && !seeking_.load() &&
             is_running_.load() && !playback_controller_.IsSeekRequested())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (!is_running_.load() || frame_generation != video_generation_.load() || playback_controller_.IsSeekRequested() ||
          frame_serial != seek_serial_.load()) {
        av_frame_free(&frame);
        continue;
      }
      if (!publishFrame(frame, frame_serial, frame_generation)) continue;
      video_recovering_.store(false);
      if (sync_trace_ && monotonicSeconds() - trace_time >= 0.5) {
        trace_time = monotonicSeconds();
        LOG(INFO) << "SYNC video time=" << std::fixed << std::setprecision(6) << trace_time
                  << " serial=" << frame_serial << " video_pts=" << pts
                  << " generation=" << frame_generation << " clock=" << audio_clock_.GetClock() << " rate=" << playbackRate();
      }
      completeSeek(frame_serial);

      if (w != last_w || h != last_h) {
        last_w = w;
        last_h = h;
        ui_.Post([callback = callbacks_.videoSize, w, h] { callback(w, h); });
      }
    }
  }

  bool publishFrame(AVFrame *frame, int serial, uint64_t generation) {
    if (!is_running_.load(std::memory_order_acquire) || !retainFrameSession(frame)) {
      av_frame_free(&frame);
      return false;
    }
    if (first_handoff_.exchange(false))
      LOG(INFO) << "STARTUP stage=first-frame-handoff ms=" << (monotonicSeconds() - startup_at_) * 1000;
    AVFrame *dropped = nullptr;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (!is_running_.load(std::memory_order_acquire) || generation != video_generation_.load() || playback_controller_.IsSeekRequested() || serial != seek_serial_.load()) {
        av_frame_free(&frame);
        return false;
      }
      dropped = pending_frame_;
      if (dropped) ++render_drops_;
      pending_frame_ = frame;
      pending_generation_ = generation;
#ifdef FFPLAYER_VALIDATION
      if (validation_video) {
        const auto timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        validation_video(timestamp * av_q2d(v_time_base_), serial, generation, audio_clock_.GetClock());
      }
#endif
    }
    av_frame_free(&dropped);

    // 合并同一事件循环周期内的多次请求，替代原先 8ms 轮询定时器
    if (!update_posted_.exchange(true, std::memory_order_acq_rel)) {
      ui_.Post(callbacks_.frameReady);
    }
    return true;
  }

  PlaybackUiDispatcher ui_;
  Callbacks callbacks_;
#ifdef FFPLAYER_VALIDATION
  std::function<void(double, int, uint64_t, double)> validation_video;
  std::function<void(double, int64_t, int, int)> validation_audio;
  std::function<void(double, double, int)> validation_audio_input;
  std::function<void(uint64_t)> validation_recovery;
  std::function<void()> validation_packet_drop;
  std::function<void(uint64_t, uint64_t, uint64_t)> validation_audio_device;
#endif
  std::atomic<double> playback_rate_{1.0};
  std::atomic<bool> seeking_{false};
  std::atomic<double> discard_before_{0.0};
  std::atomic<int> audio_drained_serial_{-1};
  double duration_ = 0;
  int video_width_ = 0, video_height_ = 0;
  bool has_video_ = false, has_audio_ = false, buffer_video_ = false;
  PacketCache::Config active_cache_config_;
  PacketCache packet_cache_;
  bool initial_network_buffer_pending_ = false;
  BufferingStats buffer_stats_;
  std::atomic<uint64_t> buffer_stats_generation_{0};
  std::atomic<int> buffer_stats_serial_{-1};
  bool sync_trace_ = false;
  std::atomic<bool> buffer_hold_{false}, read_waiting_{false};
  std::atomic<bool> audio_waiting_{false}, video_waiting_{false};
  std::atomic<int> audio_output_serial_{-1}, video_output_serial_{-1};
  std::unique_ptr<Demuxer> demuxer_;
  std::unique_ptr<Decoder> video_decoder_, audio_decoder_;
  int video_stream_index_ = -1;
  FrameQueue ready_frames_;
  std::atomic<uint64_t> video_generation_{0}, callback_drops_{0}, present_drops_{0}, render_drops_{0};
  std::atomic<bool> video_recovering_{false};
  uint64_t pending_generation_ = 0;
  Clock audio_clock_;
  std::mutex clock_update_mutex_;
  PlaybackController playback_controller_;
  std::atomic<bool> is_running_{true}, eof_reached_{false}, eof_signaled_{false};
  std::atomic<int> video_drained_serial_{-1}, seek_serial_{0};
  AVRational v_time_base_ = {1, 25}, a_time_base_ = {1, 48000};
  double v_packet_duration_ = 0, a_packet_duration_ = 0;
  std::mutex frame_mutex_;
  AVFrame *pending_frame_ = nullptr;
  std::atomic<bool> update_posted_{false};
  std::thread open_thread_, read_thread_, video_thread_, audio_thread_, present_thread_;
  double startup_at_ = 0;
  std::atomic<bool> first_handoff_{true};
  std::mutex seek_control_mutex_;
  std::atomic<bool> input_seek_pending_{false};
  bool stop_finished_ = false;
};

} // namespace ffmpeg_player
