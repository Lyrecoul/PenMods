#ifndef PLAYBACK_CONTROLLER_HPP
#define PLAYBACK_CONTROLLER_HPP

#include "FFmpegCore.hpp"
#include <atomic>
#include <mutex>

namespace ffmpeg_player {

class PlaybackController {
private:
  std::atomic<bool> paused{false};
  std::atomic<bool> seek_requested{false};
  std::atomic<double> seek_target{0.0};
  std::mutex seek_mutex;
  uint64_t seek_id = 0;

public:
  PlaybackController() = default;

  // 暂停/恢复播放
  void TogglePause() {
    paused = !paused;
    LOG(INFO) << (paused ? "Paused" : "Resumed");
  }

  void SetPause(bool pause) {
    paused = pause;
    LOG(INFO) << (paused ? "Paused" : "Resumed");
  }

  bool IsPaused() const { return paused; }

  // Seek 操作
  void RequestSeek(double seconds) {
    std::lock_guard<std::mutex> lock(seek_mutex);
    seek_target = seconds;
    ++seek_id;
    seek_requested = true;
    LOG(INFO) << "Seek requested to: " << seconds << "s";
  }

  bool IsSeekRequested() const { return seek_requested; }

  double GetSeekTarget() {
    std::lock_guard<std::mutex> lock(seek_mutex);
    return seek_target;
  }

  void ClearSeekRequest() {
    std::lock_guard<std::mutex> lock(seek_mutex);
    seek_requested = false;
  }

  std::pair<uint64_t, double> GetSeekRequest() {
    std::lock_guard<std::mutex> lock(seek_mutex);
    return {seek_id, seek_target.load()};
  }

  void CompleteSeek(uint64_t id) {
    std::lock_guard<std::mutex> lock(seek_mutex);
    if (seek_id == id)
      seek_requested = false;
  }

  // 相对 Seek（前进/后退）
  void SeekForward(double seconds, double current_time) {
    RequestSeek(current_time + seconds);
  }

  void SeekBackward(double seconds, double current_time) {
    RequestSeek(std::max(0.0, current_time - seconds));
  }

  // 重置控制器状态
  void Reset() {
    std::lock_guard<std::mutex> lock(seek_mutex);
    paused = false;
    seek_requested = false;
    seek_target = 0.0;
  }

  void SetPaused(bool m_paused) {
    paused=m_paused;
  }
};

} // namespace ffmpeg_player

#endif
