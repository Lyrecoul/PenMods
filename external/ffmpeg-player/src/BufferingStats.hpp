#ifndef BUFFERING_STATS_HPP
#define BUFFERING_STATS_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace ffmpeg_player {

// 时间戳在读包线程中记录；快照与跳转重置可从 GUI 线程调用。
class BufferingStats {
public:
  static constexpr double LocalTargetSeconds = 1.0;
  struct Snapshot {
    double end = 0;
    double duration = 0;
    double progress = 0;
    double videoEnd = 0;
    double audioEnd = 0;
    bool videoValid = false;
    bool audioValid = false;
  };

  uint64_t Reset(bool video, bool audio, double position = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    video_ = video;
    audio_ = audio;
    anchor_ = position;
    video_ranges_.clear();
    audio_ranges_.clear();
    return ++generation_;
  }

  uint64_t Generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }

  void Record(uint64_t generation, bool video, double start, double end) {
    if (!std::isfinite(start) || !std::isfinite(end) || end <= start) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_) return;
    auto &ranges = video ? video_ranges_ : audio_ranges_;
    auto it = ranges.begin();
    while (it != ranges.end() && it->end + 0.002 < start) ++it;
    double mergedStart = start;
    double mergedEnd = end;
    while (it != ranges.end() && it->start <= mergedEnd + 0.002) {
      mergedStart = std::min(mergedStart, it->start);
      mergedEnd = std::max(mergedEnd, it->end);
      it = ranges.erase(it);
    }
    ranges.insert(it, {mergedStart, mergedEnd});
  }

  Snapshot Get(double position, double target, double duration = 0) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot out;
    out.videoValid = video_ && rangeEnd(video_ranges_, position, out.videoEnd);
    out.audioValid = audio_ && rangeEnd(audio_ranges_, position, out.audioEnd);
    out.videoEnd = out.videoValid ? std::max(position, out.videoEnd) : position;
    out.audioEnd = out.audioValid ? std::max(position, out.audioEnd) : position;
    out.end = video_ && audio_ ? std::min(out.videoEnd, out.audioEnd)
             : video_ ? out.videoEnd : audio_ ? out.audioEnd : anchor_;
    out.end = std::max(position, out.end);
    if (duration > 0) out.end = std::min(duration, out.end);
    out.duration = std::max(0.0, out.end - position);
    const double boundedTarget = duration > 0 ? std::min(target, std::max(0.0, duration - position)) : target;
    out.progress = boundedTarget > 0 ? std::min(1.0, out.duration / boundedTarget) : 1.0;
    return out;
  }

private:
  struct Range { double start; double end; };

  static bool rangeEnd(const std::vector<Range> &ranges, double position, double &end) {
    bool found = false;
    for (const auto &range : ranges) {
      if (range.start <= position + 0.1 && range.end >= position - 0.000001) {
        end = std::max(end, range.end);
        found = true;
      }
    }
    return found;
  }

  mutable std::mutex mutex_;
  uint64_t generation_ = 0;
  bool video_ = false, audio_ = false;
  double anchor_ = 0;
  std::vector<Range> video_ranges_, audio_ranges_;
};

// 一秒滑动窗口，读取停顿后速度自然回落到零。时间单位为秒，速度为字节/秒。
class DownloadRate {
public:
  void Reset(double now, int64_t bytes = 0) {
    samples_.clear();
    samples_.push_back({now, bytes});
  }

  double Update(double now, int64_t bytes) {
    if (samples_.empty() || now < samples_.back().time || bytes < samples_.back().bytes) {
      Reset(now, bytes);
      return 0;
    }
    samples_.push_back({now, bytes});
    while (samples_.size() > 2 && samples_[1].time <= now - 1.0)
      samples_.pop_front();
    const auto first = samples_.front();
    const double elapsed = now - first.time;
    return elapsed > 0 ? (bytes - first.bytes) / elapsed : 0;
  }

private:
  struct Sample { double time; int64_t bytes; };
  std::deque<Sample> samples_;
};

} // namespace ffmpeg_player
#endif
