#ifndef FFMPEG_VIDEO_PLAYER_QML_HPP
#define FFMPEG_VIDEO_PLAYER_QML_HPP

// ============================================================
// FFmpeg Video Player QML Component — Single Header Library
//
// 用法 (QML):
//   import FFmpegPlayer 1.0
//   VideoPlayer {
//       anchors.fill: parent
//       source: "/path/to/video.mp4"
//       autoPlay: true
//       onPositionChanged: console.log("pos:", position)
//   }
//
// 注册 (C++):
//   #include "FFmpegVideoPlayerQml.hpp"
//   qmlRegisterType<ffmpeg_player::VideoPlayer>("FFmpegPlayer", 1, 0,
//   "VideoPlayer");
//   // 或使用 QML_ELEMENT 自动注册
//
// 视频管线：
//   读包线程 → 视频解码线程(rkmpp 硬解 / 软解，自动回退)
//            → 呈现线程(音视频同步，只交接 AVFrame，不做像素转换)
//            → 渲染线程(VideoRenderNode：GPU 做 YUV→RGB / dma-buf 零拷贝)
// ============================================================

#include "AudioDevice.hpp"
#include "AudioResampler.hpp"
#include "AudioTempo.hpp"
#include "BufferingStats.hpp"
#include "Clock.hpp"
#include "Decoder.hpp"
#include "Demuxer.hpp"
#include "FFmpegCore.hpp"
#include "FrameQueue.hpp"
#include "PacketCache.hpp"
#include "PlaybackController.hpp"
#include "PlaybackProgressOverlay.hpp"
#include "RAII_WRAPPERS_HPP"
#include "VideoRenderNode.hpp"
#include "VideoPacketDrop.hpp"
#include "SubtitleOverlay.hpp"

#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGNode>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QRegularExpression>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace ffmpeg_player {

// ============================================================
// 内部工具
// ============================================================
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

// ============================================================
// VideoPlayer — QML 播放器组件
// ============================================================
class VideoPlayer : public QQuickItem {
  Q_OBJECT
  QML_ELEMENT

  // ─── QML 属性 ──────────────────────────────
  Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
  Q_PROPERTY(QUrl audioSource READ audioSource WRITE setAudioSource NOTIFY audioSourceChanged)
  Q_PROPERTY(QVariantMap httpHeaders READ httpHeaders WRITE setHttpHeaders NOTIFY httpHeadersChanged)
  Q_PROPERTY(bool loading READ loading NOTIFY playbackStateChanged)
  Q_PROPERTY(bool seeking READ seeking NOTIFY seekingChanged)
  Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
  Q_PROPERTY(qreal bufferProgress READ bufferProgress NOTIFY bufferStatsChanged)
  Q_PROPERTY(qreal bufferedDuration READ bufferedDuration NOTIFY bufferStatsChanged)
  Q_PROPERTY(qreal bufferedPosition READ bufferedPosition NOTIFY bufferStatsChanged)
  Q_PROPERTY(qreal downloadSpeed READ downloadSpeed NOTIFY downloadSpeedChanged)
  Q_PROPERTY(bool cacheEnabled READ cacheEnabled WRITE setCacheEnabled NOTIFY cacheConfigChanged)
  Q_PROPERTY(qint64 cacheForwardBytes READ cacheForwardBytes WRITE setCacheForwardBytes NOTIFY cacheConfigChanged)
  Q_PROPERTY(qint64 cacheBackwardBytes READ cacheBackwardBytes WRITE setCacheBackwardBytes NOTIFY cacheConfigChanged)
  Q_PROPERTY(qreal networkBufferSeconds READ networkBufferSeconds WRITE setNetworkBufferSeconds NOTIFY cacheConfigChanged)
  Q_PROPERTY(qreal cachedStartPosition READ cachedStartPosition NOTIFY bufferStatsChanged)
  Q_PROPERTY(qreal cachedEndPosition READ cachedEndPosition NOTIFY bufferStatsChanged)
  Q_PROPERTY(qint64 cacheUsedBytes READ cacheUsedBytes NOTIFY bufferStatsChanged)
  Q_PROPERTY(qreal playbackRate READ playbackRate WRITE setPlaybackRate NOTIFY playbackRateChanged)
  Q_PROPERTY(qreal boostRate READ boostRate WRITE setBoostRate NOTIFY boostRateChanged)
  Q_PROPERTY(bool boosted READ boosted NOTIFY boostedChanged)
  Q_PROPERTY(
      bool autoPlay READ autoPlay WRITE setAutoPlay NOTIFY autoPlayChanged)
  Q_PROPERTY(PlaybackState playbackState READ playbackState NOTIFY
                 playbackStateChanged)
  Q_PROPERTY(qreal duration READ duration NOTIFY durationChanged)
  Q_PROPERTY(qreal position READ position WRITE seek NOTIFY positionChanged)
  Q_PROPERTY(bool progressBarEnabled READ progressBarEnabled WRITE setProgressBarEnabled NOTIFY progressBarEnabledChanged)
  Q_PROPERTY(QColor progressBarColor READ progressBarColor WRITE setProgressBarColor NOTIFY progressBarColorChanged)
  Q_PROPERTY(QColor progressBarBackgroundColor READ progressBarBackgroundColor WRITE setProgressBarBackgroundColor NOTIFY progressBarBackgroundColorChanged)
  Q_PROPERTY(qreal progressBarHeight READ progressBarHeight WRITE setProgressBarHeight NOTIFY progressBarHeightChanged)
  Q_PROPERTY(qreal progressBarPosition READ progressBarPosition WRITE setProgressBarPosition NOTIFY progressBarPositionChanged)
  Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoSizeChanged)
  Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoSizeChanged)
  Q_PROPERTY(bool downsamplingEnabled READ downsamplingEnabled WRITE setDownsamplingEnabled NOTIFY downsamplingEnabledChanged)
  Q_PROPERTY(
      FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
  Q_PROPERTY(
      int rotation READ rotation WRITE setRotation NOTIFY rotationChanged)
  Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY mediaStatusChanged)
  Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY mediaStatusChanged)
  Q_PROPERTY(QString errorString READ errorString NOTIFY errorOccurred)
  Q_PROPERTY(
      QString videoDecoder READ videoDecoder NOTIFY videoDecoderChanged)
  Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding NOTIFY
                 videoDecoderChanged)
  Q_PROPERTY(QUrl subtitleSource READ subtitleSource WRITE setSubtitleSource NOTIFY subtitleSourceChanged)
  Q_PROPERTY(bool subtitlesEnabled READ subtitlesEnabled WRITE setSubtitlesEnabled NOTIFY subtitlesEnabledChanged)
  Q_PROPERTY(qreal subtitleDelay READ subtitleDelay WRITE setSubtitleDelay NOTIFY subtitleDelayChanged)
  Q_PROPERTY(QUrl subtitleFontFile READ subtitleFontFile WRITE setSubtitleFontFile NOTIFY subtitleFontFileChanged)
  Q_PROPERTY(qreal subtitleFontScale READ subtitleFontScale WRITE setSubtitleFontScale NOTIFY subtitleFontScaleChanged)
  Q_PROPERTY(bool subtitleLoading READ subtitleLoading NOTIFY subtitleLoadingChanged)
  Q_PROPERTY(QString subtitleErrorString READ subtitleErrorString NOTIFY subtitleErrorChanged)

public:
#ifdef FFPLAYER_VALIDATION
  // Host-only observation at the actual presentation/PCM handoff, absent from
  // production builds and the QML interface. Install before open().
  std::function<void(double, int, uint64_t, double)> validation_video;
  std::function<void(double, int64_t, int, int)> validation_audio;
  std::function<void(double, double, int)> validation_audio_input;
  std::function<void(uint64_t)> validation_recovery;
  std::function<void()> validation_packet_drop;
  std::function<void(uint64_t, uint64_t, uint64_t)> validation_audio_device;
#endif
  enum PlaybackState { StoppedState, PlayingState, PausedState, LoadingState };
  Q_ENUM(PlaybackState)

  enum FillMode { PreserveAspectFit, PreserveAspectCrop, Stretch };
  Q_ENUM(FillMode)

  // ─── 构造 / 析构 ─────────────────────────────
  explicit VideoPlayer(QQuickItem *parent = nullptr)
      : QQuickItem(parent), ready_frames_(3) {
    setFlag(ItemHasContents, true);
    connect(this, &QQuickItem::smoothChanged, this, [this]() { update(); });

    FFmpegCore::Init();

    progress_overlay_ = new PlaybackProgressOverlay(this);
    connect(this, &VideoPlayer::positionChanged, this, &VideoPlayer::syncProgressBar);
    connect(this, &VideoPlayer::durationChanged, this, &VideoPlayer::syncProgressBar);
    connect(this, &VideoPlayer::playbackStateChanged, this, &VideoPlayer::syncProgressBar);

    subtitle_overlay_ = new subtitles::Overlay(this);
    subtitle_overlay_->reportError = [this](const QString& error) { setSubtitleError(error); };
    subtitle_timer_ = new QTimer(this);
    subtitle_timer_->setInterval(33);
    subtitle_timer_->setTimerType(Qt::PreciseTimer);
    connect(subtitle_timer_, &QTimer::timeout, this, &VideoPlayer::syncSubtitles);
    connect(this, &VideoPlayer::playbackStateChanged, this, &VideoPlayer::syncSubtitles);
    connect(this, &VideoPlayer::positionChanged, this, &VideoPlayer::syncSubtitles);
    connect(this, &VideoPlayer::seekingChanged, this, &VideoPlayer::syncSubtitles);
    connect(this, &VideoPlayer::videoSizeChanged, this, &VideoPlayer::syncSubtitles);
    connect(this, &VideoPlayer::fillModeChanged, this, &VideoPlayer::syncSubtitles);
    connect(this, &VideoPlayer::rotationChanged, this, &VideoPlayer::syncSubtitles);
    connect(this, &QQuickItem::visibleChanged, this, &VideoPlayer::syncSubtitles);
    connect(this, &QQuickItem::windowChanged, this, &VideoPlayer::syncSubtitles);

    // 位置更新定时器
    position_timer_ = new QTimer(this);
    position_timer_->setInterval(250);
    connect(position_timer_, &QTimer::timeout, this, [this]() {
      // 播放或暂停状态都更新位置（暂停时显示当前位置）
      if (state_ != StoppedState && audio_clock_.IsStarted()) {
        double pos = audio_clock_.GetClock();
        pos = std::max(0.0, duration_ > 0 ? std::min(pos, duration_) : pos);
        if (std::abs(pos - last_reported_pos_) > 0.1) {
          last_reported_pos_ = pos;
          emit positionChanged();
        }
      }
    });

    buffer_timer_ = new QTimer(this);
    buffer_timer_->setInterval(100);
    connect(buffer_timer_, &QTimer::timeout, this, &VideoPlayer::updateBufferingStats);

    download_speed_timer_ = new QTimer(this);
    download_speed_timer_->setInterval(500);
    connect(download_speed_timer_, &QTimer::timeout, this, &VideoPlayer::updateDownloadSpeed);
  }

  ~VideoPlayer() override {
    subtitle_loader_.stop();
    stopInternal();
    std::lock_guard<std::mutex> lock(frame_mutex_);
    av_frame_free(&pending_frame_);
  }

  VideoPlayer(const VideoPlayer &) = delete;
  VideoPlayer &operator=(const VideoPlayer &) = delete;

  // ─── 属性访问器 ─────────────────────────────
  QUrl source() const { return source_; }
  QUrl audioSource() const { return audio_source_; }
  QVariantMap httpHeaders() const { return http_headers_; }
  bool loading() const { return state_ == LoadingState; }
  bool seeking() const { return seeking_.load(std::memory_order_acquire); }
  bool buffering() const { return buffering_; }
  qreal bufferProgress() const { return buffer_progress_; }
  qreal bufferedDuration() const { return buffered_duration_; }
  qreal bufferedPosition() const { return buffered_position_; }
  bool cacheEnabled() const { return cache_config_.enabled; }
  qint64 cacheForwardBytes() const { return cache_config_.forward; }
  qint64 cacheBackwardBytes() const { return cache_config_.backward; }
  qreal networkBufferSeconds() const { return cache_config_.networkSeconds; }
  qreal cachedStartPosition() const { return cached_start_; }
  qreal cachedEndPosition() const { return cached_end_; }
  qint64 cacheUsedBytes() const { return cache_used_; }
  void setCacheEnabled(bool enabled) {
    if (cache_config_.enabled == enabled) return;
    cache_config_.enabled = enabled;
    emit cacheConfigChanged();
  }
  void setCacheForwardBytes(qint64 bytes) {
    if (bytes <= 0 || bytes > INT64_MAX - cache_config_.backward || bytes == cache_config_.forward) return;
    cache_config_.forward = bytes;
    emit cacheConfigChanged();
  }
  void setCacheBackwardBytes(qint64 bytes) {
    if (bytes < 0 || bytes > INT64_MAX - cache_config_.forward || bytes == cache_config_.backward) return;
    cache_config_.backward = bytes;
    emit cacheConfigChanged();
  }
  void setNetworkBufferSeconds(qreal seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds == cache_config_.networkSeconds) return;
    cache_config_.networkSeconds = seconds;
    emit cacheConfigChanged();
  }
  qreal downloadSpeed() const { return download_speed_; }
  qreal playbackRate() const { return playback_rate_.load(); }
  qreal boostRate() const { return boost_rate_; }
  bool boosted() const { return boosted_; }
  bool autoPlay() const { return auto_play_; }
  PlaybackState playbackState() const { return state_; }
  qreal duration() const { return duration_; }
  bool progressBarEnabled() const { return progress_bar_enabled_; }
  QColor progressBarColor() const { return progress_bar_color_; }
  QColor progressBarBackgroundColor() const { return progress_bar_background_; }
  qreal progressBarHeight() const { return progress_bar_height_; }
  qreal progressBarPosition() const { return progress_bar_position_; }

  qreal position() const {
    if (!audio_clock_.IsStarted())
      return 0.0;
    double pos = audio_clock_.GetClock();
    return std::max(0.0, duration_ > 0 ? std::min(pos, duration_) : pos);
  }

  int videoWidth() const { return video_width_; }
  int videoHeight() const { return video_height_; }
  bool downsamplingEnabled() const { return downsampling_enabled_; }
  FillMode fillMode() const { return fill_mode_; }
  int rotation() const { return rotation_angle_; }
  bool hasVideo() const { return has_video_; }
  bool hasAudio() const { return has_audio_; }
  QString errorString() const { return error_string_; }
  QString videoDecoder() const { return video_decoder_name_; }
  bool hardwareDecoding() const { return hardware_decoding_; }
  QUrl subtitleSource() const { return subtitle_source_; }
  bool subtitlesEnabled() const { return subtitles_enabled_; }
  qreal subtitleDelay() const { return subtitle_delay_; }
  QUrl subtitleFontFile() const { return subtitle_font_file_; }
  qreal subtitleFontScale() const { return subtitle_font_scale_; }
  bool subtitleLoading() const { return subtitle_loading_; }
  QString subtitleErrorString() const { return subtitle_error_; }

  // ─── QML 可调用方法 ──────────────────────────
public slots:
  void setDownsamplingEnabled(bool enabled) {
    if (downsampling_enabled_ == enabled)
      return;
    downsampling_enabled_ = enabled;
    emit downsamplingEnabledChanged();
    update(); // Also switches the current material when playback is paused.
  }

  void setProgressBarEnabled(bool enabled) {
    if (progress_bar_enabled_ == enabled) return;
    progress_bar_enabled_ = enabled;
    emit progressBarEnabledChanged();
    syncProgressBar();
  }
  void setProgressBarColor(const QColor& color) {
    if (!color.isValid() || progress_bar_color_ == color) return;
    progress_bar_color_ = color;
    emit progressBarColorChanged();
    syncProgressBar();
  }
  void setProgressBarBackgroundColor(const QColor& color) {
    if (!color.isValid() || progress_bar_background_ == color) return;
    progress_bar_background_ = color;
    emit progressBarBackgroundColorChanged();
    syncProgressBar();
  }
  void setProgressBarHeight(qreal height) {
    if (!std::isfinite(height) || height < 0 || progress_bar_height_ == height) return;
    progress_bar_height_ = height;
    emit progressBarHeightChanged();
    syncProgressBar();
  }
  void setProgressBarPosition(qreal seconds) {
    if (!std::isfinite(seconds)) return;
    seconds = seconds < 0 ? -1 : seconds;
    if (progress_bar_position_ == seconds) return;
    progress_bar_position_ = seconds;
    emit progressBarPositionChanged();
    syncProgressBar();
  }
  void setSubtitleSource(const QUrl& source) {
    if (source.isEmpty()) { clearSubtitles(); return; }
    if (subtitle_source_ == source) return;
    subtitle_source_ = source;
    const auto previousGeneration = subtitle_generation_;
    emit subtitleSourceChanged();
    if (previousGeneration != subtitle_generation_) return;
    const auto generation = beginSubtitleLoad();
    if (generation != subtitle_generation_) return;
    subtitle_loader_.request(this, source, [this, generation](subtitles::LoadResult result) {
      finishSubtitleLoad(generation, std::move(result));
    });
  }
  void setSubtitleText(const QString& text, const QString& format = QStringLiteral("ass")) {
    if (text.isEmpty()) { clearSubtitles(); return; }
    const auto previousGeneration = subtitle_generation_;
    if (!subtitle_source_.isEmpty()) { subtitle_source_.clear(); emit subtitleSourceChanged(); }
    if (previousGeneration != subtitle_generation_) return;
    const auto generation = beginSubtitleLoad();
    if (generation != subtitle_generation_) return;
    subtitle_loader_.requestText(this, text, format.toLower(), [this, generation](subtitles::LoadResult result) {
      finishSubtitleLoad(generation, std::move(result));
    });
  }
  void clearSubtitles() {
    const auto generation = ++subtitle_generation_;
    subtitle_loader_.cancel();
    subtitle_document_.reset();
    if (!subtitle_source_.isEmpty()) { subtitle_source_.clear(); emit subtitleSourceChanged(); }
    if (generation != subtitle_generation_) return;
    if (subtitle_loading_) { subtitle_loading_ = false; emit subtitleLoadingChanged(); }
    if (generation != subtitle_generation_) return;
    setSubtitleError({});
    syncSubtitles();
  }
  void setSubtitlesEnabled(bool enabled) {
    if (subtitles_enabled_ == enabled) return;
    subtitles_enabled_ = enabled;
    emit subtitlesEnabledChanged();
    syncSubtitles();
  }
  void setSubtitleDelay(qreal seconds) {
    if (!std::isfinite(seconds) || std::abs(seconds) > 86400) {
      setSubtitleError(QStringLiteral("Subtitle delay must be finite and within one day"));
      return;
    }
    if (subtitle_delay_ == seconds) return;
    subtitle_delay_ = seconds;
    emit subtitleDelayChanged();
    syncSubtitles();
  }
  void setSubtitleFontFile(const QUrl& url) {
    if (!url.isEmpty() && !url.isLocalFile() && !url.scheme().isEmpty()) {
      setSubtitleError(QStringLiteral("Subtitle font must be a local file"));
      return;
    }
    if (subtitle_font_file_ == url) return;
    subtitle_font_file_ = url;
    setSubtitleError({});
    emit subtitleFontFileChanged();
    syncSubtitles();
  }
  void setSubtitleFontScale(qreal scale) {
    if (!std::isfinite(scale) || scale < 0.25 || scale > 4.0) {
      setSubtitleError(QStringLiteral("Subtitle font scale must be between 0.25 and 4.0"));
      return;
    }
    if (subtitle_font_scale_ == scale) return;
    subtitle_font_scale_ = scale;
    emit subtitleFontScaleChanged();
    syncSubtitles();
  }
  void setPlaybackRate(qreal rate) {
    if (!std::isfinite(rate) || rate < 0.5 || rate > 2.0) {
      setError(QStringLiteral("Playback rate must be between 0.5 and 2.0"));
      return;
    }
    if (rate == playbackRate())
      return;
    playback_rate_.store(rate);
    // 有音频时由音频线程在 PCM 输出边界切换时钟；纯视频可以立即变速。
    if (!has_audio_)
      audio_clock_.SetRate(rate);
    emit playbackRateChanged();
  }

  void setBoostRate(qreal rate) {
    if (!std::isfinite(rate) || rate < 0.5 || rate > 2.0) {
      setError(QStringLiteral("Boost rate must be between 0.5 and 2.0"));
      return;
    }
    if (boost_rate_ == rate) return;
    boost_rate_ = rate;
    emit boostRateChanged();
    if (boosted_) setPlaybackRate(boost_rate_);
  }

  void beginBoost() {
    if (boosted_ || state_ != PlayingState)
      return;
    boosted_ = true;
    setPlaybackRate(boost_rate_);
    emit boostedChanged();
  }

  void endBoost() { finishBoost(); }

  void setAudioSource(const QUrl &url) {
    if (audio_source_ == url)
      return;
    stop();
    audio_source_ = url;
    emit audioSourceChanged();
  }

  void setHttpHeaders(const QVariantMap &headers) {
    if (http_headers_ == headers)
      return;
    // 禁止换行和非 token 字段名，防止错误构造 HTTP 请求。
    const QRegularExpression token(QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$"));
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
      const QString value = it.value().toString();
      if (!token.match(it.key()).hasMatch() || value.contains('\r') ||
          value.contains('\n') || value.contains(QChar(0))) {
        setError(QStringLiteral("Invalid HTTP header"));
        return;
      }
    }
    http_headers_ = headers;
    emit httpHeadersChanged();
  }

  // 一次设置所有输入，避免 autoPlay 提前打开尚未配好的独立音轨。
  void open(const QUrl &video, const QUrl &audio = QUrl(),
            const QVariantMap &headers = QVariantMap()) {
    stop();
    setHttpHeaders(headers);
    if (http_headers_ != headers)
      return;
    audio_source_ = audio;
    source_ = video;
    emit audioSourceChanged();
    emit sourceChanged();
    play();
  }

  void setSource(const QUrl &url) {
    if (source_ == url)
      return;
    source_ = url;
    emit sourceChanged();

    stop();

    if (!url.isEmpty() && auto_play_)
      play();
  }

  void setAutoPlay(bool v) {
    if (auto_play_ == v)
      return;
    auto_play_ = v;
    emit autoPlayChanged();
    if (auto_play_ && !source_.isEmpty() && state_ == StoppedState)
      play();
  }

  void setFillMode(FillMode m) {
    if (fill_mode_ == m)
      return;
    fill_mode_ = m;
    emit fillModeChanged();
    update();
  }

  void setRotation(int angle) {
    angle = ((angle % 360) + 360) % 360;
    if (rotation_angle_ == angle)
      return;
    rotation_angle_ = angle;
    emit rotationChanged();
    update();
  }

  void play() {
    if (source_.isEmpty())
      return;

    // 从暂停状态恢复（包括 EOF 暂停）
    if (state_ == PausedState) {
      // 如果是 EOF 暂停，需要先 seek 到开头或当前位置以清除 EOF 状态
      if (eof_signaled_.load(std::memory_order_relaxed)) {
        // seek 到开头重新播放
        seek(0.0);
      }
      {
        std::lock_guard<std::mutex> lock(clock_update_mutex_);
        playback_controller_.SetPaused(false);
        if (!has_audio_) audio_clock_.SetPaused(seeking_.load() || buffer_hold_.load());
      }
      setState(PlayingState);
      return;
    }

    if (state_ == PlayingState || state_ == LoadingState)
      return;

    startPlayback();
  }

  void pause() {
    if (state_ != PlayingState)
      return;
    {
      std::lock_guard<std::mutex> lock(clock_update_mutex_);
      playback_controller_.SetPaused(true);
      // Freeze the software clock at the request boundary. The output thread
      // later confirms the actual device position and applies latency.
      audio_clock_.SetPaused(true);
      video_recovering_.store(false);
    }
    setState(PausedState);
    setBuffering(seeking());
  }

  void togglePlayPause() {
    if (state_ == PlayingState)
      pause();
    else
      play();
  }

  void stop() { stopInternal(); }

  void seek(qreal seconds) {
    // 允许在暂停状态下 seek（包括 EOF 后的暂停）
    if ((state_ != PlayingState && state_ != PausedState) || !std::isfinite(seconds))
      return;

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

    last_reported_pos_ = seconds;
    starvation_since_ = 0;
    buffer_progress_ = buffered_duration_ = 0;
    buffered_position_ = seconds;
    emit bufferStatsChanged();
    setBuffering(true);
    if (notify_seeking)
      emit seekingChanged();
    emit positionChanged();
  }

  void seekForward(qreal delta = 10.0) {
    if (state_ == StoppedState)
      return;
    seek(position() + delta);
  }

  void seekBackward(qreal delta = 10.0) {
    if (state_ == StoppedState)
      return;
    seek(position() - delta);
  }

signals:
  void sourceChanged();
  void audioSourceChanged();
  void httpHeadersChanged();
  void seekingChanged();
  void bufferingChanged();
  void bufferStatsChanged();
  void cacheConfigChanged();
  void downloadSpeedChanged();
  void seekFinished(qreal position);
  void playbackRateChanged();
  void boostRateChanged();
  void boostedChanged();
  void autoPlayChanged();
  void playbackStateChanged();
  void durationChanged();
  void positionChanged();
  void progressBarEnabledChanged();
  void progressBarColorChanged();
  void progressBarBackgroundColorChanged();
  void progressBarHeightChanged();
  void progressBarPositionChanged();
  void videoSizeChanged();
  void downsamplingEnabledChanged();
  void fillModeChanged();
  void rotationChanged();
  void mediaStatusChanged();
  void errorOccurred(const QString &error);
  void endOfMedia();
  void videoDecoderChanged();
  void subtitleSourceChanged();
  void subtitlesEnabledChanged();
  void subtitleDelayChanged();
  void subtitleFontFileChanged();
  void subtitleFontScaleChanged();
  void subtitleLoadingChanged();
  void subtitleErrorChanged();

protected:
  // 渲染线程调用（GUI 线程阻塞中）。像素转换/纹理上传都在 VideoRootNode 内完成。
  QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override {
    auto *root = static_cast<render::VideoRenderSlots *>(oldNode);

    AVFrame *frame = nullptr;
    uint64_t generation;
    const int serial = seek_serial_.load();
    {
      std::lock_guard<std::mutex> frameLock(frame_mutex_);
      std::swap(frame, pending_frame_);
      generation = pending_generation_;
    }
    if (frame && generation != video_generation_.load()) av_frame_free(&frame);

    if (clear_node_.exchange(false, std::memory_order_acq_rel)) {
      delete root;
      root = nullptr;
    }

    if (!frame && !root)
      return nullptr;
    if (!root)
      root = new render::VideoRenderSlots(window());

    const QRectF rect = calculateRenderRect();
    const auto dpr = window()->effectiveDevicePixelRatio();
    if (frame) {
      root->Prepare(frame, rect, rotation_angle_, dpr, downsampling_enabled_, smooth());
      std::lock_guard<std::mutex> frameLock(frame_mutex_);
      if (generation == video_generation_.load() && serial == seek_serial_.load() &&
          !playback_controller_.IsSeekRequested() && is_running_.load()) root->Commit();
    }
    root->Update(rect, rotation_angle_, dpr, downsampling_enabled_, smooth());
    return root;
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  void geometryChange(const QRectF &new_geometry,
                      const QRectF &old_geometry) override {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    syncProgressBar();
    syncSubtitles();
    update();
  }
#else
  void geometryChanged(const QRectF &new_geometry,
                       const QRectF &old_geometry) override {
    QQuickItem::geometryChanged(new_geometry, old_geometry);
    syncProgressBar();
    syncSubtitles();
    update();
  }
#endif

private:
  static double monotonicSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void setBuffering(bool active) {
    if (buffering_ == active) return;
    buffering_ = active;
    emit bufferingChanged();
  }

  void releaseBufferHold() {
    std::lock_guard<std::mutex> lock(clock_update_mutex_);
    if (buffer_hold_.exchange(false, std::memory_order_acq_rel)) {
      packet_cache_.AcknowledgeCapacity();
      audio_clock_.SetPaused(playback_controller_.IsPaused() || seeking());
    }
  }

  void updateDownloadSpeed() {
    if (state_ == StoppedState) return;
    const double speed = download_rate_.Update(monotonicSeconds(), demuxer_ ? demuxer_->NetworkBytes() : 0);
    if (download_speed_ == speed) return;
    download_speed_ = speed;
    emit downloadSpeedChanged();
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

  void updateBufferingStats() {
    if (state_ == StoppedState) return;
    const auto session = session_id_.load();
    const double now = monotonicSeconds();
    const double pos = position();
    packet_cache_.SetPosition(pos);
    const auto stats = packet_cache_.Get(pos);
    const double target = active_cache_config_.network && active_cache_config_.enabled
        ? active_cache_config_.networkSeconds : BufferingStats::LocalTargetSeconds;
    const auto handed = buffer_stats_.Get(pos, target, duration_);
    const double videoEnd = std::max(stats.videoEnd, handed.videoEnd);
    const double audioEnd = std::max(stats.audioEnd, handed.audioEnd);
    double end = buffer_video_ && has_audio_ ? std::min(videoEnd, audioEnd)
               : buffer_video_ ? videoEnd : has_audio_ ? audioEnd : pos;
    if (duration_ > 0) end = std::min(duration_, end);
    end = std::max(pos, end);
    const double seconds = end - pos;
    // Progress always refers to the configured prefetch target, including
    // capacity and EOF exceptions.
    const double progress = std::min(1.0, seconds / target);
    const bool statsChanged = buffer_progress_ != progress || buffered_position_ != end ||
        buffered_duration_ != seconds || cached_start_ != stats.seekStart ||
        cached_end_ != stats.seekEnd || cache_used_ != stats.bytes;
    buffer_progress_ = progress;
    buffered_duration_ = seconds;
    buffered_position_ = end;
    cached_start_ = stats.seekStart;
    cached_end_ = duration_ > 0 ? std::min(duration_, stats.seekEnd) : stats.seekEnd;
    cache_used_ = stats.bytes;

    // 缺包需持续 150ms；解码慢、正常帧间隔以及片尾排空不作为网络缓冲。
    const bool eof = eof_reached_.load(std::memory_order_acquire);
    const bool waiting = read_waiting_.load(std::memory_order_acquire) &&
        ((has_audio_ && audio_waiting_.load()) || (buffer_video_ && video_waiting_.load()));
    const bool canWait = state_ == PlayingState && !seeking() && !eof;
    if (canWait && waiting) {
      if (starvation_since_ == 0) starvation_since_ = now;
    } else {
      starvation_since_ = 0;
    }
    const bool starved = canWait && starvation_since_ > 0 && now - starvation_since_ >= 0.15;
    // Either required track can starve with independent readers. The output
    // thread preserves queued PCM and confirms the actual freeze position.
    if (starved && !buffer_hold_.load()) {
      std::lock_guard<std::mutex> lock(clock_update_mutex_);
      buffer_hold_.store(true, std::memory_order_release);
      if (!has_audio_) audio_clock_.SetPaused(true);
    }

    if (buffer_hold_.load()) {
      const bool videoReady = !buffer_video_ || (has_audio_ && video_recovering_.load()) ||
          video_output_serial_.load() == seek_serial_.load() ||
          video_drained_serial_.load() == seek_serial_.load();
      const bool audioReady = !has_audio_ || audio_output_serial_.load() == seek_serial_.load() ||
          audio_drained_serial_.load() == seek_serial_.load();
      const bool startupReady = !initial_network_buffer_pending_ || seconds >= target || stats.capacity || stats.eof;
      if (!playback_controller_.IsSeekRequested() && videoReady && audioReady && startupReady) {
        LOG(INFO) << "Buffer ready: serial=" << seek_serial_.load() << " seconds=" << seconds
                  << " target=" << target << " bytes=" << stats.bytes
                  << " capacity=" << stats.capacity << " eof=" << stats.eof;
        releaseBufferHold();
        initial_network_buffer_pending_ = false;
        if (demuxer_) demuxer_->SetStartupPrefetch(false);
        starvation_since_ = 0;
      }
    }

    setBuffering(loading() || seeking() || (state_ == PlayingState && (buffer_hold_.load() || starved)));
    // QML 信号处理器可以停止或更换媒体，后续通知不能泄漏到新会话。
    if (session != session_id_.load()) return;
    if (statsChanged) emit bufferStatsChanged();
  }

  void syncProgressBar() {
    if (!progress_overlay_) return;
    const qreal barHeight = std::min(progress_bar_height_, std::max(qreal(0), height()));
    progress_overlay_->setPosition(QPointF(0, height() - barHeight));
    progress_overlay_->setSize(QSizeF(std::max(qreal(0), width()), barHeight));
    const qreal barPosition = progress_bar_position_ < 0 ? position() : progress_bar_position_;
    progress_overlay_->setProgress(duration_ > 0 ? barPosition / duration_ : 0,
                                   progress_bar_color_, progress_bar_background_);
    progress_overlay_->setVisible(progress_bar_enabled_ && duration_ > 0 &&
        barHeight > 0 && (state_ == PlayingState || state_ == PausedState));
  }

  void finishBoost() {
    if (!boosted_) return;
    boosted_ = false;
    setPlaybackRate(1.0);
    emit boostedChanged();
  }

  quint64 beginSubtitleLoad() {
    const quint64 generation = ++subtitle_generation_;
    subtitle_loader_.cancel();
    subtitle_document_.reset();
    setSubtitleError({});
    if (generation != subtitle_generation_) return generation;
    if (!subtitle_loading_) { subtitle_loading_ = true; emit subtitleLoadingChanged(); }
    syncSubtitles();
    return generation;
  }
  void finishSubtitleLoad(quint64 generation, subtitles::LoadResult result) {
    if (generation != subtitle_generation_) return;
    subtitle_document_ = std::move(result.document);
    setSubtitleError(result.error);
    if (generation != subtitle_generation_) return;
    subtitle_loading_ = false;
    emit subtitleLoadingChanged();
    syncSubtitles();
  }
  void setSubtitleError(const QString& error) {
    if (subtitle_error_ == error) return;
    subtitle_error_ = error;
    emit subtitleErrorChanged();
  }
  void syncSubtitles() {
    if (!subtitle_overlay_) return;
    const bool active = subtitles_enabled_ && subtitle_document_ && isVisible() && window() &&
                        (state_ == PlayingState || state_ == PausedState);
    if (subtitle_document_ && subtitle_default_font_.isEmpty()) subtitle_default_font_ = subtitles::defaultFont();
    const QString font = subtitle_font_file_.isEmpty() ? subtitle_default_font_ :
        subtitle_font_file_.isLocalFile() ? subtitle_font_file_.toLocalFile() : subtitle_font_file_.path();
    subtitle_overlay_->setContent(subtitle_document_, font, subtitle_font_scale_);
    const QRectF rect = render::ComputePlacement(calculateRenderRect(), rotation_angle_).tex_rect;
    subtitle_overlay_->setPosition(rect.topLeft());
    subtitle_overlay_->setSize(rect.size());
    subtitle_overlay_->setRotation(rotation_angle_);
    subtitle_overlay_->setTime(qint64(std::llround((position() - subtitle_delay_) * 1000)),
                              QSize(video_width_, video_height_));
    subtitle_overlay_->setVisible(active && !seeking());
    if (active && state_ == PlayingState) {
      if (!subtitle_timer_->isActive()) subtitle_timer_->start();
    } else {
      subtitle_timer_->stop();
    }
  }
  // 解码与呈现线程丢弃过期画面，保留最后一帧等待追赶。

  // ─── 渲染区域计算 ─────────────────────────────
  QRectF calculateRenderRect() const {
    const qreal iw = width();
    const qreal ih = height();
    if (video_width_ <= 0 || video_height_ <= 0 || iw <= 0 || ih <= 0)
      return QRectF(0, 0, iw, ih);

    if (fill_mode_ == Stretch)
      return QRectF(0, 0, iw, ih);

    const bool swap = (rotation_angle_ == 90 || rotation_angle_ == 270);
    const qreal ew = swap ? video_height_ : video_width_;
    const qreal eh = swap ? video_width_ : video_height_;
    const qreal va = ew / eh;
    const qreal ia = iw / ih;

    qreal rw, rh;
    if (fill_mode_ == PreserveAspectFit) {
      if (ia > va) {
        rh = ih;
        rw = va * rh;
      } else {
        rw = iw;
        rh = rw / va;
      }
    } else {
      if (ia > va) {
        rw = iw;
        rh = rw / va;
      } else {
        rh = ih;
        rw = va * rh;
      }
    }
    return QRectF((iw - rw) / 2.0, (ih - rh) / 2.0, rw, rh);
  }

  void setState(PlaybackState s) {
    if (state_ == s)
      return;
    state_ = s;
    emit playbackStateChanged();
  }

  void setError(const QString &msg) {
    error_string_ = msg;
    emit errorOccurred(msg);
  }

  void setVideoDecoderInfo(const QString &name, bool hw) {
    if (video_decoder_name_ == name && hardware_decoding_ == hw)
      return;
    video_decoder_name_ = name;
    hardware_decoding_ = hw;
    emit videoDecoderChanged();
  }

  void resetSyncState() {
    audio_clock_.Reset();
    audio_clock_.SetRate(playbackRate());
    playback_controller_.Reset();
  }

  void postPlaybackError(const QString &message) {
    const auto session = session_id_.load();
    QMetaObject::invokeMethod(this, [this, message, session]() {
      if (session != session_id_.load())
        return;
      stopInternal();
      setError(message);
    }, Qt::QueuedConnection);
  }

  void completeSeek(int serial) {
    if (!seeking_.load() || playback_controller_.IsSeekRequested())
      return;
    const auto session = session_id_.load();
    QMetaObject::invokeMethod(this, [this, serial, session]() {
      if (session != session_id_.load() || serial != seek_serial_.load() ||
          playback_controller_.IsSeekRequested() || !seeking_.exchange(false))
        return;
      {
        std::lock_guard<std::mutex> lock(clock_update_mutex_);
        audio_clock_.SetPaused(playback_controller_.IsPaused() || buffer_hold_.load());
      }
      emit seekingChanged();
      emit seekFinished(position());
    }, Qt::QueuedConnection);
  }

  // EOF 处理：暂停而不是停止，保持可 seek 状态
  Q_INVOKABLE void handleEndOfMedia() {
    if (state_ == StoppedState)
      return;

    // 暂停时钟
    audio_clock_.SetPaused(true);

    // 暂停播放控制器（让工作线程进入等待状态）
    playback_controller_.SetPaused(true);
    releaseBufferHold();
    setBuffering(false);

    // 先取消长按，状态/位置的 QML 回调即使调用 endBoost 也不会在 EOF 发起跳转。
    finishBoost();

    // EOF 后固定在片尾，避免最后一帧的 PTS 留在 duration 之前。
    if (duration_ > 0)
      audio_clock_.SetClock(duration_);
    last_reported_pos_ = position();
    emit positionChanged();

    // 进入暂停状态而不是停止状态
    setState(PausedState);
    if (seeking_.exchange(false)) {
      emit seekingChanged();
      emit seekFinished(position());
    }
    emit endOfMedia();
  }

  void stopInternal() {
    if (state_ == StoppedState)
      return;

    is_running_.store(false, std::memory_order_release);
    video_recovering_.store(false);
    ++session_id_;

    // 唤醒所有阻塞的队列操作
    packet_cache_.Abort();
    ready_frames_.Abort();

    joinAllThreads();
    buffer_timer_->stop();
    download_speed_timer_->stop();

    // 重置所有状态
    packet_cache_.Clear();
    ready_frames_.Clear();
    resetSyncState();
    buffer_hold_.store(false);
    audio_waiting_.store(false);
    video_waiting_.store(false);
    audio_output_serial_.store(-1);
    video_output_serial_.store(-1);
    read_waiting_.store(false);
    starvation_since_ = 0;
    buffer_video_ = false;
    buffer_progress_ = buffered_duration_ = buffered_position_ = download_speed_ = 0;
    cached_start_ = cached_end_ = 0;
    cache_used_ = 0;

    demuxer_.reset();
    video_decoder_.reset();
    audio_decoder_.reset();

    has_video_ = false;
    has_audio_ = false;
    video_width_ = 0;
    video_height_ = 0;
    video_stream_index_ = -1;
    duration_ = 0.0;
    last_reported_pos_ = 0.0;
    eof_reached_.store(false, std::memory_order_relaxed);
    eof_signaled_.store(false, std::memory_order_relaxed);
    video_drained_serial_.store(-1, std::memory_order_relaxed);

    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      av_frame_free(&pending_frame_);
    }
    clear_node_.store(true, std::memory_order_release);

    if (QThread::currentThread() == thread()) {
      position_timer_->stop();
    } else {
      QMetaObject::invokeMethod(position_timer_, "stop",
                                Qt::BlockingQueuedConnection);
    }

    setVideoDecoderInfo(QString(), false);
    setState(StoppedState);
    setBuffering(false);
    emit bufferStatsChanged();
    emit downloadSpeedChanged();
    if (seeking_.exchange(false))
      emit seekingChanged();
    endBoost();
    emit positionChanged();
    emit durationChanged();
    emit videoSizeChanged();
    emit mediaStatusChanged();
    update();
  }

  // ─── 启动播放 ─────────────────────────────
  void startPlayback() {
    startup_at_ = monotonicSeconds();
    first_handoff_.store(true);
    error_string_.clear();
    const auto inputPath = [](const QUrl &url) {
      return (url.isLocalFile() ? url.toLocalFile()
              : url.scheme().isEmpty() ? url.path()
                                      : url.toString(QUrl::FullyEncoded)).toStdString();
    };
    const std::string path = inputPath(source_);
    const std::string audio_path = inputPath(audio_source_);
    active_cache_config_ = cache_config_;
    active_cache_config_.network = (!source_.scheme().isEmpty() && !source_.isLocalFile()) ||
        (!audio_source_.isEmpty() && !audio_source_.scheme().isEmpty() && !audio_source_.isLocalFile());
    std::string headers;
    for (auto it = http_headers_.cbegin(); it != http_headers_.cend(); ++it)
      headers += it.key().toStdString() + ": " + it.value().toString().toStdString() + "\r\n";
    demuxer_ = std::make_unique<Demuxer>();
    is_running_.store(true, std::memory_order_release);
    download_rate_.Reset(monotonicSeconds());
    const auto session = ++session_id_;
    setState(LoadingState);
    if (session != session_id_.load()) return;
    setBuffering(true);
    if (session != session_id_.load()) return;
    buffer_timer_->start();
    download_speed_timer_->start();
    open_thread_ = std::thread([this, path, audio_path, headers, session]() {
      const bool ok = demuxer_->Open(path, headers, &is_running_, audio_path);
      auto prepared = std::make_shared<PreparedDecoders>();
      if (ok && is_running_.load() && session == session_id_.load()) {
        auto *fmt = demuxer_->GetRawContext();
        prepared->index = demuxer_->GetVideoStreamIndex();
        prepared->attached = prepared->index < 0 && demuxer_->HasAttachedPicture();
        if (prepared->attached) {
          for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
              prepared->index = static_cast<int>(i);
              break;
            }
        }
        prepared->video = std::make_unique<Decoder>();
        prepared->audio = std::make_unique<Decoder>();
        prepared->hasVideo = prepared->index >= 0 &&
            prepared->video->Init(fmt->streams[prepared->index]->codecpar, !prepared->attached);
        if (is_running_.load() && session == session_id_.load())
          prepared->hasAudio = demuxer_->GetAudioStreamIndex() >= 0 &&
              prepared->audio->Init(demuxer_->GetAudioCodecParameters(), false);
        demuxer_->Trace("decoder-init");
      }
      const QString openError = QString::fromStdString(demuxer_->Error());
      QMetaObject::invokeMethod(this, [this, ok, session, prepared, openError]() {
        if (session != session_id_)
          return;
        if (open_thread_.joinable())
          open_thread_.join();
        if (!ok) {
          stopInternal();
          setError(openError);
          return;
        }
        finishStartPlayback(*prepared);
      }, Qt::QueuedConnection);
    });
  }

  struct PreparedDecoders {
    std::unique_ptr<Decoder> video, audio;
    int index = -1;
    bool attached = false, hasVideo = false, hasAudio = false;
  };

  void finishStartPlayback(PreparedDecoders &prepared) {
    video_recovering_.store(false);
    callback_drops_.store(0);
    present_drops_.store(0);
    render_drops_.store(0);
    sync_trace_ = qEnvironmentVariableIsSet("FFPLAYER_SYNC_TRACE");
    input_seek_pending_.store(false);
    demuxer_->SetReadInterrupt([this]() { return input_seek_pending_.load(); });
    AVFormatContext *fmt_ctx = demuxer_->GetRawContext();
    duration_ = fmt_ctx->duration > 0 ? static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE : 0.0;
    // Unknown-duration network inputs use sequential buffering; live rewind is
    // intentionally outside the compressed VOD cache contract.
    if (active_cache_config_.network && duration_ <= 0) active_cache_config_.enabled = false;
    emit durationChanged();

    video_decoder_ = std::move(prepared.video);
    audio_decoder_ = std::move(prepared.audio);
    has_video_ = prepared.hasVideo;
    has_audio_ = prepared.hasAudio;

    // 视频流：普通视频优先尝试硬解；专辑封面（附加图片）直接软解
    video_stream_index_ = prepared.index;
    const bool attached_pic = prepared.attached;

    // 时间基
    v_time_base_ = {1, 25};
    a_time_base_ = {1, 48000};

    if (video_stream_index_ >= 0) {
      AVStream *vst = fmt_ctx->streams[video_stream_index_];
      v_time_base_ = vst->time_base;
    }
    if (!has_video_)
      video_stream_index_ = -1;
    buffer_video_ = has_video_ && !attached_pic;

    if (!has_video_ && !has_audio_) {
      stopInternal();
      setError(QStringLiteral("No decodable streams found"));
      return;
    }

    emit mediaStatusChanged();

    if (has_video_) {
      video_width_ = video_decoder_->GetContext()->width;
      video_height_ = video_decoder_->GetContext()->height;
      emit videoSizeChanged();
      setVideoDecoderInfo(QString::fromLatin1(video_decoder_->Name()),
                          video_decoder_->IsHardware());
    }

    if (has_audio_) {
      a_time_base_ = demuxer_->GetAudioTimeBase();
    }

    // 在工作线程启动前缓存回退时长，读包线程不读取正在解码的 AVCodecContext。
    v_packet_duration_ = a_packet_duration_ = 0;
    if (has_video_) {
      const auto rate = av_guess_frame_rate(fmt_ctx, fmt_ctx->streams[video_stream_index_], nullptr);
      if (rate.num > 0 && rate.den > 0) v_packet_duration_ = av_q2d(av_inv_q(rate));
    }
    if (has_audio_) {
      const auto *ctx = audio_decoder_->GetContext();
      if (ctx->sample_rate > 0) a_packet_duration_ = static_cast<double>(ctx->frame_size) / ctx->sample_rate;
    }

    // 重置队列和状态
    ready_frames_.Resume();
    packet_cache_.Open(active_cache_config_, has_video_, has_audio_, buffer_video_, seek_serial_.load());
    buffer_stats_serial_.store(seek_serial_.load(), std::memory_order_release);
    buffer_stats_generation_.store(buffer_stats_.Reset(buffer_video_, has_audio_, position()),
                                   std::memory_order_release);
    ready_frames_.Clear();
    resetSyncState();
    buffer_hold_.store(true);
    initial_network_buffer_pending_ = active_cache_config_.network && active_cache_config_.enabled;
    demuxer_->SetStartupPrefetch(initial_network_buffer_pending_);
    audio_clock_.SetPaused(true);
    eof_reached_.store(false, std::memory_order_relaxed);
    eof_signaled_.store(false, std::memory_order_relaxed);
    video_drained_serial_.store(-1, std::memory_order_relaxed);
    audio_drained_serial_.store(-1, std::memory_order_relaxed);
    discard_before_.store(0.0);
    is_running_.store(true, std::memory_order_release);

    // 启动工作线程
    read_thread_ = std::thread(&VideoPlayer::readThreadFunc, this);
    if (has_video_)
      video_thread_ = std::thread(&VideoPlayer::videoThreadFunc, this);
    if (has_audio_)
      audio_thread_ = std::thread(&VideoPlayer::audioThreadFunc, this);
    present_thread_ = std::thread(&VideoPlayer::presentThreadFunc, this);

    position_timer_->start();

    setState(PlayingState);
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

  // ============================================================
  // 工作线程实现
  // 读包/解码/呈现线程不再绑核：原先读包与呈现都钉在 CPU0，
  // 与 GUI/渲染线程争抢；交给调度器分配更均衡。
  // ============================================================

  // ─── 读取线程 ─────────────────────────────
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

  // 解码帧送入呈现队列（队列满时阻塞，响应 seek/停止）
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

  void notifyVideoDecoderChanged() {
    const QString name = QString::fromLatin1(video_decoder_->Name());
    const bool hw = video_decoder_->IsHardware();
    const auto session = session_id_.load();
    QMetaObject::invokeMethod(
        this, [this, name, hw, session]() {
          if (session == session_id_.load())
            setVideoDecoderInfo(name, hw);
        },
        Qt::QueuedConnection);
  }

  // ─── 视频解码线程 ─────────────────────────────
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

    const FrameCallback on_frame = [&](AVFrame *frame) -> bool {
      ++decoded_frames;
      if (decoded_frames == 1) LOG(INFO) << "STARTUP stage=first-video ms=" << (monotonicSeconds() - startup_at_) * 1000;
      if (seek_serial_.load(std::memory_order_acquire) != last_seek_serial ||
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

      int ret = video_decoder_->Decode(is_flush ? nullptr : pkt, on_frame);
      av_packet_free(&pkt);
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
          ret = video_decoder_->Decode(nullptr, on_frame);
      }

      if ((ret == AVERROR_EOF || is_flush) &&
          last_seek_serial == seek_serial_.load() && !playback_controller_.IsSeekRequested())
        video_drained_serial_.store(last_seek_serial, std::memory_order_release);
    }
  }

  // ─── 音频解码线程 ─────────────────────────────
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
      audio_decoder_->Decode(is_flush ? nullptr : pkt, on_frame);
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

  // ─── 帧呈现线程 ──────────────────────────────
  // 只负责音视频同步与交接帧，像素处理全部留给渲染线程/GPU。
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
            const auto session = session_id_.load();
            const int serial = eof_serial;
            QMetaObject::invokeMethod(this, [this, session, serial]() {
              if (session == session_id_.load() && serial == seek_serial_.load() &&
                  eof_reached_.load() && !playback_controller_.IsSeekRequested())
                handleEndOfMedia();
            }, Qt::QueuedConnection);
          }

          // 等待 seek 或停止
          while (is_running_.load(std::memory_order_relaxed) &&
                 seek_serial_.load(std::memory_order_acquire) == eof_serial &&
                 eof_reached_.load(std::memory_order_relaxed) &&
                 !playback_controller_.IsSeekRequested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
        const auto session = session_id_.load();
        QMetaObject::invokeMethod(
            this,
            [this, w, h, session]() {
              if (session != session_id_.load())
                return;
              if (video_width_ != w || video_height_ != h) {
                video_width_ = w;
                video_height_ = h;
                emit videoSizeChanged();
              }
            },
            Qt::QueuedConnection);
      }
    }
  }

  // 把帧交给渲染线程；渲染线程来不及取的旧帧直接释放
  bool publishFrame(AVFrame *frame, int serial, uint64_t generation) {
    if (first_handoff_.exchange(false))
      LOG(INFO) << "STARTUP stage=first-frame-handoff ms=" << (monotonicSeconds() - startup_at_) * 1000;
    AVFrame *dropped = nullptr;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (generation != video_generation_.load() || playback_controller_.IsSeekRequested() || serial != seek_serial_.load()) {
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
      QMetaObject::invokeMethod(
          this,
          [this]() {
            update_posted_.store(false, std::memory_order_release);
            update();
          },
          Qt::QueuedConnection);
    }
    return true;
  }

  // ─── 成员变量 ─────────────────────────────

  // QML 属性
  QUrl source_;
  bool downsampling_enabled_ = true;
  QUrl subtitle_source_, subtitle_font_file_;
  bool subtitles_enabled_ = true, subtitle_loading_ = false;
  qreal subtitle_delay_ = 0, subtitle_font_scale_ = 1;
  QString subtitle_error_, subtitle_default_font_;
  quint64 subtitle_generation_ = 0;
  subtitles::DocumentPtr subtitle_document_;
  subtitles::Loader subtitle_loader_;
  subtitles::Overlay* subtitle_overlay_ = nullptr;
  QTimer* subtitle_timer_ = nullptr;
  QUrl audio_source_;
  QVariantMap http_headers_;
  std::atomic<uint64_t> session_id_{0};
  std::atomic<double> playback_rate_{1.0};
  bool boosted_ = false;
  qreal boost_rate_ = 2.0;
  bool progress_bar_enabled_ = false;
  QColor progress_bar_color_{QStringLiteral("#80cfff")};
  QColor progress_bar_background_{Qt::transparent};
  qreal progress_bar_height_ = 2;
  qreal progress_bar_position_ = -1;
  PlaybackProgressOverlay* progress_overlay_ = nullptr;
  std::atomic<bool> seeking_{false};
  std::atomic<double> discard_before_{0.0};
  std::atomic<int> audio_drained_serial_{-1};
  bool auto_play_ = false;
  PlaybackState state_ = StoppedState;
  qreal duration_ = 0.0;
  int video_width_ = 0;
  int video_height_ = 0;
  FillMode fill_mode_ = PreserveAspectFit;
  int rotation_angle_ = 0;
  bool has_video_ = false;
  bool has_audio_ = false;
  QString error_string_;
  QString video_decoder_name_;
  bool hardware_decoding_ = false;
  double last_reported_pos_ = 0.0;
  bool buffering_ = false, buffer_video_ = false;
  qreal buffer_progress_ = 0, buffered_duration_ = 0, buffered_position_ = 0, download_speed_ = 0;
  double starvation_since_ = 0;
  PacketCache::Config cache_config_, active_cache_config_;
  PacketCache packet_cache_;
  qreal cached_start_ = 0, cached_end_ = 0;
  qint64 cache_used_ = 0;
  bool initial_network_buffer_pending_ = false;
  BufferingStats buffer_stats_;
  std::atomic<uint64_t> buffer_stats_generation_{0};
  std::atomic<int> buffer_stats_serial_{-1};
  bool sync_trace_ = false;
  DownloadRate download_rate_;
  std::atomic<bool> buffer_hold_{false};
  std::atomic<bool> read_waiting_{false};
  std::atomic<bool> audio_waiting_{false}, video_waiting_{false};
  std::atomic<int> audio_output_serial_{-1}, video_output_serial_{-1};

  // FFmpeg 组件
  std::unique_ptr<Demuxer> demuxer_;
  std::unique_ptr<Decoder> video_decoder_;
  std::unique_ptr<Decoder> audio_decoder_;
  int video_stream_index_ = -1;

  // 队列
  FrameQueue ready_frames_;
  std::atomic<uint64_t> video_generation_{0}, callback_drops_{0}, present_drops_{0}, render_drops_{0};
  std::atomic<bool> video_recovering_{false};
  uint64_t pending_generation_ = 0;

  // 同步 & 控制
  Clock audio_clock_; // 主时钟：有音频时由音频驱动，纯视频时由首帧启动
  std::mutex clock_update_mutex_;
  PlaybackController playback_controller_;
  std::atomic<bool> is_running_{false};
  std::atomic<bool> eof_reached_{false};
  std::atomic<bool> eof_signaled_{false};
  std::atomic<int> video_drained_serial_{-1};
  std::atomic<int> seek_serial_{0};

  // 时间基
  AVRational v_time_base_ = {1, 25};
  AVRational a_time_base_ = {1, 48000};
  double v_packet_duration_ = 0, a_packet_duration_ = 0;

  // 呈现 → 渲染 交接
  std::mutex frame_mutex_;
  AVFrame *pending_frame_ = nullptr;
  std::atomic<bool> update_posted_{false};
  std::atomic<bool> clear_node_{false};

  // 定时器
  QTimer *position_timer_ = nullptr;
  QTimer *buffer_timer_ = nullptr;
  QTimer *download_speed_timer_ = nullptr;

  // 工作线程
  std::thread open_thread_;
  double startup_at_ = 0;
  std::atomic<bool> first_handoff_{true};
  std::mutex seek_control_mutex_;
  std::atomic<bool> input_seek_pending_{false};
  std::thread read_thread_;
  std::thread video_thread_;
  std::thread audio_thread_;
  std::thread present_thread_;
};

} // namespace ffmpeg_player

#endif // FFMPEG_VIDEO_PLAYER_QML_HPP
