#ifndef CLOCK_HPP
#define CLOCK_HPP

#include "AudioTimeline.hpp"
#include <mutex>
#include <chrono>

namespace ffmpeg_player {

class Clock {
private:
  mutable std::recursive_mutex mutex_;
  double last_updated = 0;
  double last_pts = 0;
  double last_latency = 0;
  double rate_ = 1.0;
  AudioTimeline buffered_audio_;
  bool paused = false;
  bool is_set = false;

  double GetSystemTime() const {
    // 使用 steady_clock，不受系统时间调整影响
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration)
               .count() /
           1000000.0;
  }

public:
  // 更新时钟（带延迟补偿）
  // latency_seconds: 当前缓冲区中尚未播放的数据延迟（秒）
  void SetClock(double new_pts, double latency_seconds = 0) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 当前的真实 PTS 是 解码 PTS 减去 还在缓冲区没播出的时间
    last_pts = new_pts - latency_seconds * rate_;
    last_updated = GetSystemTime();
    last_latency = latency_seconds;
    buffered_audio_.Clear();
    is_set = true;
  }

  // 每次 PCM 成功入队后调用。旧倍速 PCM 播完前，不能将整个延迟乘以新倍速。
  void SetAudioClock(double end_pts, double duration, double rate,
                     double latency_seconds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    buffered_audio_.Append(duration, rate);
    buffered_audio_.TrimTo(latency_seconds);
    last_pts = end_pts - buffered_audio_.MediaDuration();
    last_updated = GetSystemTime();
    last_latency = latency_seconds;
    rate_ = rate;
    is_set = true;
  }

  // 获取当前播放到了第几秒
  // 如果暂停，返回最后一次记录的 PTS，不随系统时间增加
  double GetClock() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_set)
      return 0; // 如果音频没开始，返回 0
    if (paused)
      return last_pts;
    return last_pts + buffered_audio_.Advance(GetSystemTime() - last_updated, rate_);
  }

  // 获取最新的音频缓冲区延迟（秒）
  double GetLatency() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_latency;
  }

  void SetRate(double rate) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last_pts = GetClock();
    rate_ = rate;
    last_updated = GetSystemTime();
    buffered_audio_.Clear();
  }

  // 处理暂停/恢复时的时钟补偿
  void SetPaused(bool p) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (paused == p)
      return;
    const double now = GetSystemTime();
    if (p && is_set) {
      const double elapsed = now - last_updated;
      last_pts += buffered_audio_.Advance(elapsed, rate_);
      buffered_audio_.TrimTo(buffered_audio_.Duration() - elapsed);
    }
    last_updated = now;
    paused = p;
  }

  bool IsStarted() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return is_set;
  }

  // 重置时钟状态
  void Reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last_updated = 0;
    last_pts = 0;
    last_latency = 0;
    buffered_audio_.Clear();
    paused = false;
    is_set = false;
  }
};

} // namespace ffmpeg_player
#endif
