#ifndef CLOCK_HPP
#define CLOCK_HPP

#include <mutex>
#include <chrono>

namespace ffmpeg_player {

class Clock {
private:
  mutable std::recursive_mutex mutex_;
  double pts_drift = 0;
  double last_pts = 0;
  double last_latency = 0;
  double rate_ = 1.0;
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
    double system_time = GetSystemTime();
    // 当前的真实 PTS 是 解码 PTS 减去 还在缓冲区没播出的时间
    double real_pts = new_pts - latency_seconds * rate_;

    this->pts_drift = real_pts - system_time * rate_;
    this->last_pts = real_pts;
    this->last_latency = latency_seconds; // 记录延迟
    this->is_set = true;                  // 标记时钟已启动
  }

  // 获取当前播放到了第几秒
  // 如果暂停，返回最后一次记录的 PTS，不随系统时间增加
  double GetClock() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_set)
      return 0; // 如果音频没开始，返回 0
    if (paused)
      return last_pts;
    return pts_drift + GetSystemTime() * rate_;
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
    pts_drift = last_pts - GetSystemTime() * rate_;
  }

  // 处理暂停/恢复时的时钟补偿
  void SetPaused(bool p) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (paused == p)
      return;
    if (p) {
      last_pts = GetClock(); // 记录暂停瞬间的时间
    } else {
      // 恢复播放时，用记录的 last_pts 重新校准 drift
      if (is_set) {
        pts_drift = last_pts - GetSystemTime() * rate_;
      }
    }
    paused = p;
  }

  bool IsStarted() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return is_set;
  }

  // 重置时钟状态
  void Reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pts_drift = 0;
    last_pts = 0;
    last_latency = 0;
    paused = false;
    is_set = false;
  }
};

} // namespace ffmpeg_player
#endif
