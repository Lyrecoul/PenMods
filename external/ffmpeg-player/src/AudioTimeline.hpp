#pragma once

#include <algorithm>
#include <deque>

namespace ffmpeg_player {

// 尚未播出的 PCM：duration 是输出秒数，乘以各段倍速才是媒体秒数。
class AudioTimeline {
  struct Segment {
    double duration;
    double rate;
  };
  std::deque<Segment> segments_;
  double duration_ = 0.0;

public:
  void Clear() {
    segments_.clear();
    duration_ = 0.0;
  }

  void Append(double duration, double rate) {
    if (duration <= 0.0)
      return;
    if (!segments_.empty() && segments_.back().rate == rate)
      segments_.back().duration += duration;
    else
      segments_.push_back({duration, rate});
    duration_ += duration;
  }

  // 根据设备实际延迟移除已经播出的前缀；保留跨越倍速边界的尾部。
  void TrimTo(double duration) {
    double consumed = std::max(0.0, duration_ - std::max(0.0, duration));
    while (!segments_.empty() && consumed > 0.0) {
      auto &front = segments_.front();
      const double part = std::min(consumed, front.duration);
      consumed -= part;
      duration_ -= part;
      front.duration -= part;
      if (front.duration <= 0.0)
        segments_.pop_front();
    }
    if (segments_.empty())
      duration_ = 0.0;
  }

  double Duration() const { return duration_; }

  double MediaDuration() const {
    double result = 0.0;
    for (const auto &segment : segments_)
      result += segment.duration * segment.rate;
    return result;
  }

  // 在下一次音频回调前，时钟也要按排队的 PCM 边界切换倍速。
  double Advance(double elapsed, double tail_rate) const {
    double result = 0.0;
    elapsed = std::max(0.0, elapsed);
    for (const auto &segment : segments_) {
      const double part = std::min(elapsed, segment.duration);
      result += part * segment.rate;
      elapsed -= part;
      if (elapsed <= 0.0)
        return result;
    }
    return result + elapsed * tail_rate;
  }
};

} // namespace ffmpeg_player
