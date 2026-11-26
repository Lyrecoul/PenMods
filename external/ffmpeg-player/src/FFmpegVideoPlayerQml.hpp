#ifndef FFMPEG_VIDEO_PLAYER_QML_HPP
#define FFMPEG_VIDEO_PLAYER_QML_HPP

// ============================================================
// FFmpeg Video Player QML Component
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
#include "PlaybackSession.hpp"
#ifdef FFPLAYER_SHUTDOWN_DIAGNOSTICS
#include "ShutdownEventProbe.hpp"
#endif
#include <QPointer>

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
      : QQuickItem(parent) {
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
      if (state_ != StoppedState && playback_ && playback_->audio_clock_.IsStarted()) {
        double pos = playback_->audio_clock_.GetClock();
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
    ShutdownTrace trace("player-destroy", this);
    if (playback_) traceShutdownWindow();
    // QQuickItem emits windowChanged while detaching in its base destructor.
    // Its signals must no longer call slots that access VideoPlayer members.
    disconnect(this, nullptr, this, nullptr);
    position_timer_->stop();
    buffer_timer_->stop();
    download_speed_timer_->stop();
    subtitle_timer_->stop();
    subtitle_overlay_->reportError = {};
    subtitle_loader_.stop();
    PlaybackSession::Retire(std::move(playback_));
  }

  VideoPlayer(const VideoPlayer &) = delete;
  VideoPlayer &operator=(const VideoPlayer &) = delete;

  // ─── 属性访问器 ─────────────────────────────
  QUrl source() const { return source_; }
  QUrl audioSource() const { return audio_source_; }
  QVariantMap httpHeaders() const { return http_headers_; }
  bool loading() const { return state_ == LoadingState; }
  bool seeking() const { return playback_ && playback_->seeking_.load(std::memory_order_acquire); }
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
    return playback_ ? playback_->position() : 0.0;
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
    if (rate == playbackRate()) return;
    playback_rate_.store(rate);
    if (playback_) {
      playback_->playback_rate_.store(rate);
      // PCM output applies audio rate changes at the existing sample boundary.
      if (!has_audio_) playback_->audio_clock_.SetRate(rate);
    }
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
    if (source_.isEmpty()) return;
    const auto playback = playback_;
    const QPointer<VideoPlayer> guard(this);
    if (state_ == PausedState && playback) {
      if (playback->eof_signaled_.load()) {
        seek(0.0);
        if (!guard || playback != playback_) return;
      }
      {
        std::lock_guard<std::mutex> lock(playback->clock_update_mutex_);
        playback->playback_controller_.SetPaused(false);
        if (!has_audio_)
          playback->audio_clock_.SetPaused(seeking() || playback->buffer_hold_.load());
      }
      setState(PlayingState);
      return;
    }
    if (state_ == PlayingState || state_ == LoadingState) return;
    startPlayback();
  }

  void pause() {
    const auto playback = playback_;
    const QPointer<VideoPlayer> guard(this);
    if (state_ != PlayingState || !playback) return;
    {
      std::lock_guard<std::mutex> lock(playback->clock_update_mutex_);
      playback->playback_controller_.SetPaused(true);
      playback->audio_clock_.SetPaused(true);
      playback->video_recovering_.store(false);
    }
    setState(PausedState);
    if (guard && playback == playback_) setBuffering(seeking());
  }

  void togglePlayPause() {
    if (state_ == PlayingState)
      pause();
    else
      play();
  }

  void stop() { stopInternal(); }

  void seek(qreal seconds) {
    const auto playback = playback_;
    const QPointer<VideoPlayer> guard(this);
    if (!playback || (state_ != PlayingState && state_ != PausedState) || !std::isfinite(seconds))
      return;
    seconds = std::max(0.0, duration_ > 0 ? std::min(seconds, duration_) : seconds);
    const bool notify = playback->seek(seconds);
    last_reported_pos_ = seconds;
    starvation_since_ = 0;
    buffer_progress_ = buffered_duration_ = 0;
    buffered_position_ = seconds;
    emit bufferStatsChanged();
    if (!guard || playback != playback_) return;
    setBuffering(true);
    if (!guard || playback != playback_) return;
    if (notify) emit seekingChanged();
    if (!guard || playback != playback_) return;
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
    const auto playback = playback_;
    AVFrame *frame = nullptr;
    uint64_t generation = 0;
    const int serial = playback ? playback->seek_serial_.load() : 0;
    if (playback) {
      std::lock_guard<std::mutex> lock(playback->frame_mutex_);
      std::swap(frame, playback->pending_frame_);
      generation = playback->pending_generation_;
    }
    if (frame && generation != playback->video_generation_.load()) av_frame_free(&frame);
    if (clear_node_.exchange(false, std::memory_order_acq_rel) || !playback) {
      delete root;
      root = nullptr;
    }
    if (!playback || (!frame && !root)) return nullptr;
    if (!root) root = new render::VideoRenderSlots(window());
    const QRectF rect = calculateRenderRect();
    const auto dpr = window()->effectiveDevicePixelRatio();
    if (frame) {
      root->Prepare(frame, rect, rotation_angle_, dpr, downsampling_enabled_, smooth());
      std::lock_guard<std::mutex> lock(playback->frame_mutex_);
      if (generation == playback->video_generation_.load() && serial == playback->seek_serial_.load() &&
          !playback->playback_controller_.IsSeekRequested() && playback->is_running_.load())
        root->Commit();
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
    if (!playback_) return;
    std::lock_guard<std::mutex> lock(playback_->clock_update_mutex_);
    if (playback_->buffer_hold_.exchange(false, std::memory_order_acq_rel)) {
      playback_->packet_cache_.AcknowledgeCapacity();
      playback_->audio_clock_.SetPaused(playback_->playback_controller_.IsPaused() || seeking());
    }
  }

  void updateDownloadSpeed() {
    if (state_ == StoppedState || !playback_) return;
    const double speed = download_rate_.Update(monotonicSeconds(), playback_->demuxer_ ? playback_->demuxer_->NetworkBytes() : 0);
    if (download_speed_ == speed) return;
    download_speed_ = speed;
    emit downloadSpeedChanged();
  }

  void updateBufferingStats() {
    if (state_ == StoppedState || state_ == LoadingState || !playback_) return;
    const auto session = session_id_.load();
    const QPointer<VideoPlayer> guard(this);
    const double now = monotonicSeconds();
    const double pos = position();
    playback_->packet_cache_.SetPosition(pos);
    const auto stats = playback_->packet_cache_.Get(pos);
    const double target = playback_->active_cache_config_.network && playback_->active_cache_config_.enabled
        ? playback_->active_cache_config_.networkSeconds : BufferingStats::LocalTargetSeconds;
    const auto handed = playback_->buffer_stats_.Get(pos, target, duration_);
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
    const bool eof = playback_->eof_reached_.load(std::memory_order_acquire);
    const bool waiting = playback_->read_waiting_.load(std::memory_order_acquire) &&
        ((has_audio_ && playback_->audio_waiting_.load()) || (buffer_video_ && playback_->video_waiting_.load()));
    const bool canWait = state_ == PlayingState && !seeking() && !eof;
    if (canWait && waiting) {
      if (starvation_since_ == 0) starvation_since_ = now;
    } else {
      starvation_since_ = 0;
    }
    const bool starved = canWait && starvation_since_ > 0 && now - starvation_since_ >= 0.15;
    // Either required track can starve with independent readers. The output
    // thread preserves queued PCM and confirms the actual freeze position.
    if (starved && !playback_->buffer_hold_.load()) {
      std::lock_guard<std::mutex> lock(playback_->clock_update_mutex_);
      playback_->buffer_hold_.store(true, std::memory_order_release);
      if (!has_audio_) playback_->audio_clock_.SetPaused(true);
    }

    if (playback_->buffer_hold_.load()) {
      const bool videoReady = !buffer_video_ || (has_audio_ && playback_->video_recovering_.load()) ||
          playback_->video_output_serial_.load() == playback_->seek_serial_.load() ||
          playback_->video_drained_serial_.load() == playback_->seek_serial_.load();
      const bool audioReady = !has_audio_ || playback_->audio_output_serial_.load() == playback_->seek_serial_.load() ||
          playback_->audio_drained_serial_.load() == playback_->seek_serial_.load();
      const bool startupReady = !playback_->initial_network_buffer_pending_ || seconds >= target || stats.capacity || stats.eof;
      if (!playback_->playback_controller_.IsSeekRequested() && videoReady && audioReady && startupReady) {
        LOG(INFO) << "Buffer ready: serial=" << playback_->seek_serial_.load() << " seconds=" << seconds
                  << " target=" << target << " bytes=" << stats.bytes
                  << " capacity=" << stats.capacity << " eof=" << stats.eof;
        releaseBufferHold();
        playback_->initial_network_buffer_pending_ = false;
        if (playback_->demuxer_) playback_->demuxer_->SetStartupPrefetch(false);
        starvation_since_ = 0;
      }
    }

    setBuffering(loading() || seeking() || (state_ == PlayingState && (playback_->buffer_hold_.load() || starved)));
    // QML 信号处理器可以停止或更换媒体，后续通知不能泄漏到新会话。
    if (!guard || session != session_id_.load()) return;
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

  void completeSeek(int serial) {
    const auto playback = playback_;
    const QPointer<VideoPlayer> guard(this);
    if (!playback || serial != playback->seek_serial_.load() ||
        playback->playback_controller_.IsSeekRequested() || !playback->seeking_.exchange(false))
      return;
    {
      std::lock_guard<std::mutex> lock(playback->clock_update_mutex_);
      playback->audio_clock_.SetPaused(playback->playback_controller_.IsPaused() ||
                                      playback->buffer_hold_.load());
    }
    emit seekingChanged();
    if (guard && playback == playback_) emit seekFinished(position());
  }

  // EOF 处理：暂停而不是停止，保持可 seek 状态
  Q_INVOKABLE void handleEndOfMedia() {
    const auto playback = playback_;
    const QPointer<VideoPlayer> guard(this);
    if (state_ == StoppedState || !playback)
      return;

    // 暂停时钟
    playback_->audio_clock_.SetPaused(true);

    // 暂停播放控制器（让工作线程进入等待状态）
    playback_->playback_controller_.SetPaused(true);
    releaseBufferHold();
    setBuffering(false);
    if (!guard || playback != playback_) return;

    // 先取消长按，状态/位置的 QML 回调即使调用 endBoost 也不会在 EOF 发起跳转。
    finishBoost();
    if (!guard || playback != playback_) return;

    // EOF 后固定在片尾，避免最后一帧的 PTS 留在 duration 之前。
    if (duration_ > 0)
      playback_->audio_clock_.SetClock(duration_);
    last_reported_pos_ = position();
    emit positionChanged();
    if (!guard || playback != playback_) return;

    // 进入暂停状态而不是停止状态
    setState(PausedState);
    if (!guard || playback != playback_) return;
    if (playback_->seeking_.exchange(false)) {
      emit seekingChanged();
      if (!guard || playback != playback_) return;
      emit seekFinished(position());
      if (!guard || playback != playback_) return;
    }
    emit endOfMedia();
  }

  void stopInternal() {
    if (state_ == StoppedState && !playback_) return;
    ShutdownTrace trace("player-stop", this);
    traceShutdownWindow();
    const auto stopId = ++session_id_;
    const bool notifySeeking = seeking();
    const bool notifyBuffering = buffering_;
    const bool notifyBoost = boosted_;
    const bool notifyDecoder = !video_decoder_name_.isEmpty() || hardware_decoding_;
    const bool notifyState = state_ != StoppedState;
    PlaybackSession::Retire(std::move(playback_));
    position_timer_->stop();
    buffer_timer_->stop();
    download_speed_timer_->stop();
    state_ = StoppedState;
    buffering_ = buffer_video_ = false;
    boosted_ = false;
    if (notifyBoost) playback_rate_.store(1.0);
    buffer_progress_ = buffered_duration_ = buffered_position_ = download_speed_ = 0;
    cached_start_ = cached_end_ = 0;
    cache_used_ = 0;
    starvation_since_ = 0;
    has_video_ = has_audio_ = false;
    video_width_ = video_height_ = 0;
    duration_ = last_reported_pos_ = 0;
    video_decoder_name_.clear();
    hardware_decoding_ = false;
    clear_node_.store(true, std::memory_order_release);
    update();

    // QML handlers may immediately open another source or destroy this item.
    // Finish the reset before notifying, then stop notifying a retired state.
    const QPointer<VideoPlayer> guard(this);
    trace.Step("stop-state-reset");
    const auto current = [guard, stopId] {
      return guard && guard->session_id_.load() == stopId;
    };
    if (notifyState) emit playbackStateChanged();
    if (!current()) return;
    if (notifyDecoder) emit videoDecoderChanged();
    if (!current()) return;
    if (notifyBuffering) emit bufferingChanged();
    if (!current()) return;
    emit bufferStatsChanged();
    if (!current()) return;
    emit downloadSpeedChanged();
    if (!current()) return;
    if (notifySeeking) emit seekingChanged();
    if (!current()) return;
    if (notifyBoost) {
      emit playbackRateChanged();
      if (!current()) return;
      emit boostedChanged();
      if (!current()) return;
    }
    emit positionChanged();
    if (!current()) return;
    emit durationChanged();
    if (!current()) return;
    emit videoSizeChanged();
    if (!current()) return;
    emit mediaStatusChanged();
  }

  void traceShutdownWindow() {
    auto *target = window();
    if (!ShutdownTrace::Enabled() || !target) return;
    // Survive the player/page: a fast stop() says nothing about the following
    // scene-graph synchronization or the first frame showing the previous page.
#ifdef FFPLAYER_SHUTDOWN_DIAGNOSTICS
    auto *probe = new ShutdownEventProbe(target);
#else
    auto *probe = new QObject(target);
#endif
    const double start = ShutdownTrace::Now();
    ShutdownTrace::Event("window-probe-start", target, start);
    // An in-flight frame can swap just after stop(), before the close is
    // synchronized. Only report an ordered sync/render/swap sequence.
    const auto phase = std::make_shared<std::atomic<unsigned>>(0);
    const auto watch = [probe, target, start, phase](auto signal, unsigned step, const char *stage) {
      QObject::connect(target, signal, probe, [phase, step, target, start, stage] {
        unsigned expected = step;
        if (phase->compare_exchange_strong(expected, step + 1))
          ShutdownTrace::Event(stage, target, start);
      }, Qt::DirectConnection);
    };
    watch(&QQuickWindow::beforeSynchronizing, 0, "before-sync");
    watch(&QQuickWindow::afterSynchronizing, 1, "after-sync");
    watch(&QQuickWindow::afterRendering, 2, "after-render");
    watch(&QQuickWindow::frameSwapped, 3, "frame-swapped");
    QTimer::singleShot(0, probe, [target, start] {
      ShutdownTrace::Event("next-gui-event", target, start);
    });
    auto *heartbeat = new QTimer(probe);
    heartbeat->setInterval(16);
    QObject::connect(heartbeat, &QTimer::timeout, probe, [target, previous = start]() mutable {
      const double now = ShutdownTrace::Now();
      if (now - previous >= 50) ShutdownTrace::Event("gui-gap", target, previous);
      previous = now;
    });
    heartbeat->start();
    QTimer::singleShot(2000, probe, &QObject::deleteLater);
  }

  // ─── 启动播放 ─────────────────────────────
  void startPlayback() {
    error_string_.clear();
    const auto inputPath = [](const QUrl &url) {
      return (url.isLocalFile() ? url.toLocalFile()
              : url.scheme().isEmpty() ? url.path()
                                      : url.toString(QUrl::FullyEncoded)).toStdString();
    };
    auto config = cache_config_;
    config.network = (!source_.scheme().isEmpty() && !source_.isLocalFile()) ||
        (!audio_source_.isEmpty() && !audio_source_.scheme().isEmpty() && !audio_source_.isLocalFile());
    std::string headers;
    for (auto it = http_headers_.cbegin(); it != http_headers_.cend(); ++it)
      headers += it.key().toStdString() + ": " + it.value().toString().toStdString() + "\r\n";
    const auto id = ++session_id_;
    PlaybackSession::Callbacks callbacks;
    callbacks.prepared = [this, id] {
      if (id == session_id_.load() && playback_) finishStartPlayback();
    };
    callbacks.frameReady = [this, id] {
      if (id != session_id_.load() || !playback_) return;
      playback_->update_posted_.store(false, std::memory_order_release);
      update();
    };
    callbacks.error = [this, id](const QString &message) {
      if (id != session_id_.load()) return;
      const QPointer<VideoPlayer> guard(this);
      stopInternal();
      if (guard && id + 1 == session_id_.load()) setError(message);
    };
    callbacks.seekFinished = [this, id](int serial) {
      if (id == session_id_.load()) completeSeek(serial);
    };
    callbacks.endOfMedia = [this, id](int serial) {
      if (id != session_id_.load() || !playback_) return;
      if (serial == playback_->seek_serial_.load() && playback_->eof_reached_.load() &&
          !playback_->playback_controller_.IsSeekRequested())
        handleEndOfMedia();
    };
    callbacks.videoSize = [this, id](int width, int height) {
      if (id != session_id_.load()) return;
      if (video_width_ != width || video_height_ != height) {
        video_width_ = width;
        video_height_ = height;
        emit videoSizeChanged();
      }
    };
    callbacks.decoderChanged = [this, id](const QString &name, bool hardware) {
      if (id == session_id_.load()) setVideoDecoderInfo(name, hardware);
    };
    const auto playback = PlaybackSession::Create(config, playbackRate(),
        PlaybackUiDispatcher(this), std::move(callbacks));
#ifdef FFPLAYER_VALIDATION
    playback->validation_video = validation_video;
    playback->validation_audio = validation_audio;
    playback->validation_audio_input = validation_audio_input;
    playback->validation_recovery = validation_recovery;
    playback->validation_packet_drop = validation_packet_drop;
    playback->validation_audio_device = validation_audio_device;
#endif
    playback_ = playback;
    download_rate_.Reset(monotonicSeconds());
    playback->open(inputPath(source_), inputPath(audio_source_), headers);
    const QPointer<VideoPlayer> guard(this);
    setState(LoadingState);
    if (!guard || playback != playback_) return;
    setBuffering(true);
    if (!guard || playback != playback_) return;
    buffer_timer_->start();
    download_speed_timer_->start();
  }

  void finishStartPlayback() {
    const auto playback = playback_;
    duration_ = playback->duration_;
    has_video_ = playback->has_video_;
    has_audio_ = playback->has_audio_;
    buffer_video_ = playback->buffer_video_;
    video_width_ = playback->video_width_;
    video_height_ = playback->video_height_;
    const QString decoder = has_video_ ? QString::fromLatin1(playback->video_decoder_->Name()) : QString();
    const bool hardware = has_video_ && playback->video_decoder_->IsHardware();
    const QPointer<VideoPlayer> guard(this);
    emit durationChanged();
    if (!guard || playback != playback_) return;
    emit mediaStatusChanged();
    if (!guard || playback != playback_) return;
    emit videoSizeChanged();
    if (!guard || playback != playback_) return;
    setVideoDecoderInfo(decoder, hardware);
    if (!guard || playback != playback_) return;
    playback->startWorkers();
    position_timer_->start();
    setState(PlayingState);
  }

  // QML state is independent of the retired worker session.
  QUrl source_, audio_source_;
  QVariantMap http_headers_;
  std::shared_ptr<PlaybackSession> playback_;
  std::atomic<uint64_t> session_id_{0};
  std::atomic<double> playback_rate_{1.0};
  bool downsampling_enabled_ = true;
  QUrl subtitle_source_, subtitle_font_file_;
  bool subtitles_enabled_ = true, subtitle_loading_ = false;
  qreal subtitle_delay_ = 0, subtitle_font_scale_ = 1;
  QString subtitle_error_, subtitle_default_font_;
  quint64 subtitle_generation_ = 0;
  subtitles::DocumentPtr subtitle_document_;
  subtitles::Loader subtitle_loader_;
  subtitles::Overlay *subtitle_overlay_ = nullptr;
  QTimer *subtitle_timer_ = nullptr;
  bool boosted_ = false;
  qreal boost_rate_ = 2.0;
  bool progress_bar_enabled_ = false;
  QColor progress_bar_color_{QStringLiteral("#80cfff")};
  QColor progress_bar_background_{Qt::transparent};
  qreal progress_bar_height_ = 2, progress_bar_position_ = -1;
  PlaybackProgressOverlay *progress_overlay_ = nullptr;
  bool auto_play_ = false;
  PlaybackState state_ = StoppedState;
  qreal duration_ = 0;
  int video_width_ = 0, video_height_ = 0;
  FillMode fill_mode_ = PreserveAspectFit;
  int rotation_angle_ = 0;
  bool has_video_ = false, has_audio_ = false;
  QString error_string_, video_decoder_name_;
  bool hardware_decoding_ = false;
  double last_reported_pos_ = 0;
  bool buffering_ = false, buffer_video_ = false;
  qreal buffer_progress_ = 0, buffered_duration_ = 0, buffered_position_ = 0, download_speed_ = 0;
  double starvation_since_ = 0;
  PacketCache::Config cache_config_;
  qreal cached_start_ = 0, cached_end_ = 0;
  qint64 cache_used_ = 0;
  DownloadRate download_rate_;
  std::atomic<bool> clear_node_{false};
  QTimer *position_timer_ = nullptr, *buffer_timer_ = nullptr, *download_speed_timer_ = nullptr;
};

} // namespace ffmpeg_player

#endif // FFMPEG_VIDEO_PLAYER_QML_HPP
