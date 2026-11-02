#ifndef PACKET_CACHE_HPP
#define PACKET_CACHE_HPP

#include "FFmpegCore.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <iterator>
#include <mutex>
#include <map>

namespace ffmpeg_player {

// Shared compressed storage. Each decoder owns a cursor, not a second packet queue.
// All indices live in Entry; allocations are charged before accepting a packet.
class PacketCache {
public:
  struct Config {
    static constexpr double LocalTargetSeconds = 1.0;
    bool enabled = true;
    int64_t forward = 30LL << 20;
    int64_t backward = 15LL << 20;
    double networkSeconds = 30;
    bool network = false;
  };
  struct Snapshot {
    double end = 0, seconds = 0;
    double videoEnd = 0, audioEnd = 0;
    double seekStart = 0, seekEnd = 0;
    int64_t bytes = 0, forwardBytes = 0, backwardBytes = 0, transientBytes = 0;
    bool videoValid = false, audioValid = false, capacity = false, eof = false;
  };

  void Open(Config config, bool video, bool audio, bool timedVideo, int serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    config_ = config;
    streams_[0].active = video;
    streams_[1].active = audio;
    timed_video_ = timedVideo;
    serial_ = serial;
    aborted_ = false;
    position_ = 0;
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    changed_.notify_all();
  }
  void Abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    changed_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(10));
  }
  void SetPosition(double position) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::abs(position - position_) < 0.05) return;
    accountMovementLocked(position);
    position_ = position;
    accountPositionLocked(true);
    trimLocked(false);
    changed_.notify_all();
  }
  void End() {
    std::lock_guard<std::mutex> lock(mutex_);
    eof_ = true;
    changed_.notify_all();
  }
  void AcknowledgeCapacity() {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = false;
  }

  bool WantRead() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (eof_ || aborted_) return false;
    if (!config_.enabled) return true;
    const auto video = currentRange(streams_[0], position_);
    const auto audio = currentRange(streams_[1], position_);
    const double end = (timed_video_ && !video.valid) || (streams_[1].active && !audio.valid) ? position_
        : timed_video_ && streams_[1].active ? std::min(video.end, audio.end)
        : timed_video_ ? video.end : audio.end;
    const double target = config_.network ? config_.networkSeconds : Config::LocalTargetSeconds;
    if (!config_.network && end - position_ >= target) return false;
    if (used_ - backward_bytes_ >= config_.forward || used_ >= cacheLimit()) {
      capacity_ = true;
      return false;
    }
    return true;
  }

  // Moves the reference only on success. A packet larger than the whole budget
  // uses one transient handoff, is never retained, and is excluded from cache bytes.
  bool Push(AVPacket *packet, bool video, AVRational tb, double fallbackDuration) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_) return false;
    auto &stream = streams_[video ? 0 : 1];
    const auto packetDts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    const int streamIndex = video ? 0 : 1;
    if (refilling_[streamIndex]) {
      if (packetDts != AV_NOPTS_VALUE &&
          packetDts * av_q2d(tb) < refill_from_[streamIndex] - 0.000001) {
        av_packet_unref(packet); // already retained prefix from the demux keyframe
        return true;
      }
      if (packetDts != AV_NOPTS_VALUE)
        refilling_[streamIndex] = false;
    }
    if (refill_needed_) return false;
    const int64_t cost = packetCost(packet);
    const int64_t limit = config_.enabled ? cacheLimit() : (1LL << 20);
    const bool transient = cost > (config_.enabled ? config_.forward : limit);
    trimLocked(false);
    if (!config_.enabled && unreadCount(stream) >= (video ? 20u : 80u)) {
      capacity_ = true;
      return false;
    }
    if (transient) {
      if (transient_) {
        capacity_ = true;
        return false;
      }
    } else {
      while (used_ + cost > limit && trimLocked(true)) {}
    }
    if ((!transient && (used_ + cost > limit ||
         (config_.enabled && used_ - backward_bytes_ + cost > config_.forward))) || (transient && transient_)) {
      capacity_ = true;
      return false;
    }
    stream.entries.emplace_back();
    if (stream.read == stream.entries.end()) stream.read = std::prev(stream.entries.end());
    auto &entry = stream.entries.back();
    entry.id = stream.next++;
    entry.cost = transient ? 0 : cost;
    entry.transientCost = transient ? cost : 0;
    entry.transient = transient;
    entry.key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    const int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    const int64_t dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : pts;
    const double length = packet->duration > 0 ? packet->duration * av_q2d(tb) : fallbackDuration;
    entry.reliable = pts != AV_NOPTS_VALUE && dts != AV_NOPTS_VALUE && length > 0;
    entry.start = pts == AV_NOPTS_VALUE ? position_ : pts * av_q2d(tb);
    entry.end = entry.start + std::max(0.0, length);
    const double decodeTime = dts == AV_NOPTS_VALUE ? 0 : dts * av_q2d(tb);
    entry.dts = decodeTime;
    entry.duration = length;
    entry.reliable = entry.reliable && std::isfinite(entry.start) && std::isfinite(entry.end);
    if (!entry.reliable || !stream.reliable ||
        decodeTime < stream.lastDts - 0.001 ||
        decodeTime - stream.lastDts > stream.lastDuration + std::max(0.002, stream.lastDuration * 0.1))
      ++stream.run;
    entry.run = stream.run;
    stream.ids.emplace(entry.id, std::prev(stream.entries.end()));
    stream.ends.emplace(std::make_pair(entry.end, entry.id), &entry);
    if (entry.reliable && !transient) {
      auto &run = stream.runs[entry.run];
      run.starts.emplace(std::make_pair(entry.start, entry.id), &entry);
      run.ends.emplace(std::make_pair(entry.end, entry.id), &entry);
      if (entry.key) run.keys.emplace(std::make_pair(entry.start, entry.id), &entry);
      auto next = run.coverage.lower_bound(entry.start);
      if (next != run.coverage.begin() && std::prev(next)->second + 0.002 >= entry.start) --next;
      double start = entry.start, end = entry.end;
      while (next != run.coverage.end() && next->first <= end + 0.002) {
        start = std::min(start, next->first);
        end = std::max(end, next->second);
        next = run.coverage.erase(next);
      }
      run.coverage.emplace(start, end);
    }
    stream.reliable = entry.reliable && !transient;
    stream.lastDts = decodeTime;
    stream.lastDuration = length;
    av_packet_move_ref(&entry.packet, packet);
    used_ += entry.cost;
    entry.behind = entry.end <= position_;
    if (entry.behind) backward_bytes_ += entry.cost;
    auto &range = stream.buffered;
    if (entry.reliable && !transient) {
      if (range.valid && range.run == entry.run) {
        range.start = std::min(range.start, entry.start);
        range.end = std::max(range.end, entry.end);
      } else if (entry.start <= position_ + 0.1 && entry.end >= position_) {
        range = {entry.start, entry.end, true, entry.run};
      }
    }
    transient_ = transient_ || transient;
    transient_bytes_ += entry.transientCost;
    capacity_ = capacity_ || transient;
    changed_.notify_all();
    return true;
  }

  template <typename Cancel>
  bool Pop(bool video, AVPacket *&packet, int &serial, Cancel cancel) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto &stream = streams_[video ? 0 : 1];
    changed_.wait_for(lock, std::chrono::milliseconds(20), [&] {
      return aborted_ || cancel() || stream.cursor < stream.next || (eof_ && !stream.flushed);
    });
    if (aborted_ || cancel()) return false;
    if (stream.read != stream.entries.end()) {
      const auto it = stream.read;
      packet = av_packet_clone(&it->packet);
      if (!packet) return false;
      ++stream.read;
      stream.lastPopped = it->id;
      stream.cursor = it->id + 1;
      serial = serial_;
      if (!config_.enabled || !config_.network || it->transient) eraseLocked(stream, it);
      changed_.notify_all();
      return true;
    }
    if (eof_ && !stream.flushed) {
      packet = av_packet_alloc();
      if (!packet) return false;
      stream.flushed = true;
      serial = serial_;
      return true;
    }
    return false;
  }

  // Called only by the reader, while the seek request prevents cursor consumption.
  // A hit leaves the demuxer's read position and EOF state untouched.
  bool Seek(double target, int serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t video = 0, audio = 0;
    if (!config_.enabled ||
        (timed_video_ && !findStart(streams_[0], target, true, video)) ||
        (streams_[1].active && !findStart(streams_[1], target, false, audio)))
      return false;
    if (streams_[0].active && !timed_video_) return false; // attached picture needs demux replay
    // Simulate tail eviction before changing cursors, serial, EOF, or packet
    // ownership, so a failed seek has no observable partial state.
    int64_t simulatedUsed = used_;
    int64_t simulatedBackward = 0;
    for (const auto &stream : streams_)
      for (const auto &entry : stream.entries)
        if (entry.end <= target) simulatedBackward += entry.cost;
    const uint64_t cursors[] = {video, audio};
    while (simulatedUsed - simulatedBackward > config_.forward) {
      const Stream *tail = nullptr;
      const Entry *tailEntry = nullptr;
      for (int i = 0; i < 2; ++i) {
        const auto &stream = streams_[i];
        if (stream.entries.empty()) continue;
        const auto &entry = stream.entries.back();
        if (entry.id < cursors[i] || entry.start <= target + 0.1) continue;
        if (!tailEntry || entry.end > tailEntry->end) {
          tail = &stream;
          tailEntry = &entry;
        }
      }
      if (!tail) return false;
      simulatedUsed -= tailEntry->cost;
      if (tailEntry->end <= target) simulatedBackward -= tailEntry->cost;
    }
    if (timed_video_) setCursorLocked(streams_[0], video);
    if (streams_[1].active) setCursorLocked(streams_[1], audio);
    serial_ = serial;
    for (auto &stream : streams_) stream.flushed = false;
    capacity_ = false;
    position_ = target;
    accountPositionLocked();
    // A backward jump reclassifies retained future packets. Evict the far tail
    // before publishing the new window; never borrow the history allowance.
    while (used_ - backward_bytes_ > config_.forward) {
      Stream *tail = nullptr;
      for (auto &candidate : streams_) {
        if (candidate.entries.empty()) continue;
        const auto &entry = candidate.entries.back();
        if (entry.id < candidate.cursor || entry.start <= target + 0.1) continue;
        if (!tail || entry.end > tail->entries.back().end) tail = &candidate;
      }
      if (!tail) return false;
      if (!refill_needed_) {
        for (int i = 0; i < 2; ++i)
          refill_from_[i] = streams_[i].lastDts + streams_[i].lastDuration;
      }
      const auto it = std::prev(tail->entries.end());
      const int index = tail == &streams_[0] ? 0 : 1;
      refill_from_[index] = std::min(refill_from_[index], it->dts);
      tail->next = it->id;
      eraseLocked(*tail, it);
      if (!tail->entries.empty()) {
        const auto &last = tail->entries.back();
        tail->lastDts = last.dts;
        tail->lastDuration = last.duration;
        tail->run = last.run;
        tail->reliable = last.reliable;
      }
      refill_needed_ = true;
      eof_ = false;
    }
    changed_.notify_all();
    return true;
  }

  bool NeedsRefill() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return refill_needed_;
  }
  bool BeginRefill(double &target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!refill_needed_) return false;
    target = timed_video_ && streams_[1].active ? std::min(refill_from_[0], refill_from_[1])
        : timed_video_ ? refill_from_[0] : refill_from_[1];
    refill_needed_ = false;
    for (int i = 0; i < 2; ++i)
      refilling_[i] = streams_[i].active;
    return true;
  }

  void Reset(double target, int serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    position_ = target;
    serial_ = serial;
    changed_.notify_all();
  }

  // Rewind the video cursor to a retained keyframe instead of storing another GOP.
  bool ReplayVideo(int serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &stream = streams_[0];
    uint64_t key = 0, run = 0;
    for (const auto &entry : stream.entries) {
      if (entry.id > stream.lastPopped) break;
      if (!entry.reliable || entry.transient || (run && run != entry.run)) key = 0;
      run = entry.run;
      if (entry.key && !entry.transient) key = entry.id;
    }
    if (!key || serial != serial_ || !config_.enabled) return false;
    setCursorLocked(stream, key);
    stream.flushed = false;
    changed_.notify_all();
    return true;
  }

  Snapshot Get(double position) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshotLocked(position);
  }
  // Advance only unread video packets. Leave the next independently decodable
  // keyframe at/after target for Pop; audio and the global seek serial are intact.
  // Repeated calls also discard a GOP incrementally when the key is not read yet.
  bool AdvanceVideo(double target, int serial, uint64_t *skipped = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_ || serial != serial_) return false;
    auto &stream = streams_[0];
    while (stream.read != stream.entries.end()) {
      const auto it = stream.read;
      if (it->key && it->reliable && it->start >= target) {
        changed_.notify_all();
        return true;
      }
      stream.cursor = it->id + 1;
      ++stream.read;
      if (skipped) ++*skipped;
      // Network history remains a complete decode prefix for a later user
      // seek. Removing the middle of a retained run would advertise a false hit.
      if (!config_.enabled || !config_.network || it->transient) eraseLocked(stream, it);
    }
    changed_.notify_all();
    return false;
  }
  bool Empty(bool video) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto &stream = streams_[video ? 0 : 1];
    return stream.cursor >= stream.next;
  }

private:
  struct Entry {
    AVPacket packet;
    uint64_t id = 0, run = 0;
    int64_t cost = 0, transientCost = 0;
    double start = 0, end = 0, dts = 0, duration = 0;
    bool key = false, reliable = false, transient = false, behind = false;
    Entry() { av_init_packet(&packet); packet.data = nullptr; packet.size = 0; }
    ~Entry() { av_packet_unref(&packet); }
    Entry(const Entry &) = delete;
    Entry &operator=(const Entry &) = delete;
  };
  struct Range { double start = 0, end = 0; bool valid = false; uint64_t run = 0; };
  using TimeIndex = std::map<std::pair<double, uint64_t>, Entry *>;
  struct Run { TimeIndex starts, ends, keys; std::map<double, double> coverage; };
  struct Stream {
    std::list<Entry> entries;
    std::list<Entry>::iterator read;
    uint64_t next = 1, cursor = 1, lastPopped = 0, run = 0;
    bool active = false, flushed = false, reliable = false;
    double lastDts = 0, lastDuration = 0;
    Range buffered;
    std::map<uint64_t, std::list<Entry>::iterator> ids;
    std::map<uint64_t, Run> runs;
    TimeIndex ends;
    Stream() : read(entries.end()) {}
  };

  static int64_t packetCost(const AVPacket *packet) {
    // Includes the list links, embedded time/key/run index, AVBuffer metadata,
    // padding and conservative allocator overhead for every owned allocation.
    int64_t cost = sizeof(Entry) + 2 * sizeof(void *) + 32;
    // Five tree nodes plus a worst-case single-entry run and allocator headers.
    cost += 6 * (sizeof(std::pair<double, uint64_t>) + 5 * sizeof(void *) + 32)
        + sizeof(Run) + 6 * sizeof(void *) + 32;
    cost += packet->buf ? packet->buf->size : static_cast<int64_t>(packet->size) + AV_INPUT_BUFFER_PADDING_SIZE;
    cost += sizeof(AVBufferRef) + 64 + 32;
    cost += packet->side_data_elems * (sizeof(AVPacketSideData) + 32);
    for (int i = 0; i < packet->side_data_elems; ++i)
      cost += packet->side_data[i].size + AV_INPUT_BUFFER_PADDING_SIZE + 32;
    return cost;
  }
  static size_t unreadCount(const Stream &stream) {
    return stream.next - stream.cursor;
  }
  void eraseLocked(Stream &stream, std::list<Entry>::iterator it) {
    used_ -= it->cost;
    if (it->behind) backward_bytes_ -= it->cost;
    if (it->transient) transient_ = false;
    transient_bytes_ -= it->transientCost;
    if (stream.read == it) ++stream.read;
    stream.ids.erase(it->id);
    stream.ends.erase({it->end, it->id});
    auto run = stream.runs.find(it->run);
    if (run != stream.runs.end()) {
      run->second.starts.erase({it->start, it->id});
      run->second.ends.erase({it->end, it->id});
      run->second.keys.erase({it->start, it->id});
      if (run->second.starts.empty()) stream.runs.erase(run);
      else {
        // Retention removes complete decode prefixes. Clip their presentation
        // coverage to the earliest remaining PTS (including reordered B frames).
        auto &coverage = run->second.coverage;
        const double start = run->second.starts.begin()->first.first;
        while (!coverage.empty() && coverage.begin()->second <= start) coverage.erase(coverage.begin());
        if (!coverage.empty() && coverage.begin()->first < start) {
          const double end = coverage.begin()->second;
          coverage.erase(coverage.begin());
          coverage.emplace(start, end);
        }
        const double end = run->second.ends.rbegin()->first.first;
        auto beyond = coverage.lower_bound(end);
        coverage.erase(beyond, coverage.end());
        if (!coverage.empty() && coverage.rbegin()->second > end) coverage.rbegin()->second = end;
      }
    }
    stream.entries.erase(it);
  }
  static void setCursorLocked(Stream &stream, uint64_t id) {
    stream.cursor = id;
    auto found = stream.ids.lower_bound(id);
    stream.read = found == stream.ids.end() ? stream.entries.end() : found->second;
  }
  void accountMovementLocked(double next) {
    for (auto &stream : streams_) {
      auto it = stream.ends.upper_bound({std::min(position_, next), UINT64_MAX});
      const auto end = stream.ends.upper_bound({std::max(position_, next), UINT64_MAX});
      for (; it != end; ++it) {
        auto &entry = *it->second;
        const bool behind = entry.end <= next;
        if (entry.behind != behind) backward_bytes_ += behind ? entry.cost : -entry.cost;
        entry.behind = behind;
      }
    }
  }
  void accountPositionLocked() {
    accountPositionLocked(false);
  }
  void accountPositionLocked(bool preserveLocalRange) {
    if (!preserveLocalRange) backward_bytes_ = 0;
    for (auto &stream : streams_) {
      if (!preserveLocalRange) for (auto &entry : stream.entries) {
        entry.behind = entry.end <= position_;
        if (entry.behind) backward_bytes_ += entry.cost;
      }
      if (!preserveLocalRange || config_.network)
        stream.buffered = rangeAt(stream, position_, false);
    }
  }
  Range currentRange(const Stream &stream, double position) const {
    Range range = config_.network ? rangeAt(stream, position, false) : stream.buffered;
    if (!range.valid || range.start > position + 0.1 || range.end < position - 0.000001)
      return {};
    return range;
  }
  void discardConsumedLocked(Stream &stream) {
    for (auto it = stream.entries.begin(); it != stream.entries.end();) {
      if (it->id < stream.cursor) {
        auto consumed = it++;
        eraseLocked(stream, consumed);
      } else {
        ++it;
      }
    }
    stream.buffered = rangeAt(stream, position_, false);
  }
  bool trimLocked(bool pressure) {
    int64_t history = backward_bytes_;
    const int64_t budget = config_.enabled && config_.network ? config_.backward : 0;
    if (!pressure && history <= budget) return false;
    bool removed = false, step = false;
    // Drop a consumed prefix up to the next keyframe. If the complete GOP cannot
    // fit, the remaining non-key prefix stays sequential-only until the next key.
    auto &video = streams_[0];
    auto &audio = streams_[1];
    do {
      step = false;
      while (!video.entries.empty()) {
        auto it = video.entries.begin();
        if (it->id >= video.cursor || it->end > position_) break;
        history -= it->cost;
        eraseLocked(video, it);
        removed = step = true;
        if (!video.entries.empty() && video.entries.front().key) break;
      }
      // Preserve audio preroll for the oldest retained video key, when possible.
      double audioBoundary = position_ - 0.1;
      if (timed_video_ && !video.entries.empty())
        audioBoundary = std::min(audioBoundary, video.entries.front().start - 0.1);
      while (!audio.entries.empty()) {
        auto it = audio.entries.begin();
        if (it->id >= audio.cursor || it->end > (pressure ? position_ : audioBoundary)) break;
        history -= it->cost;
        eraseLocked(audio, it);
        removed = step = true;
        if (pressure || history <= budget) break;
      }
    } while (!pressure && step && history > budget);
    if (removed)
      for (auto &stream : streams_) stream.buffered = rangeAt(stream, position_, false);
    return removed;
  }

  // A run is broken by a missing timestamp, backwards DTS, or a timestamp gap.
  // Never bridge such a gap using only the furthest PTS.
  static Range rangeAt(const Stream &stream, double position, bool keyRequired) {
    Range result;
    for (const auto &item : stream.runs) {
      const auto &run = item.second;
      const auto &starts = keyRequired ? run.keys : run.starts;
      if (starts.empty()) continue;
      const double allowed = position <= 0.1 ? 0.1 : position + 0.000001;
      auto covered = run.coverage.upper_bound(allowed);
      if (covered == run.coverage.begin()) continue;
      --covered;
      double start = covered->first;
      const double end = covered->second;
      if (keyRequired) {
        auto key = starts.lower_bound({start - 0.000001, 0});
        if (key == starts.end()) continue;
        start = key->first.first;
      }
      if (start <= allowed && end >= position - 0.000001)
        result = {start, end, true, item.first};
    }
    return result;
  }
  static bool findStart(const Stream &stream, double target, bool video, uint64_t &id) {
    const auto range = rangeAt(stream, target, video);
    if (!range.valid) return false;
    const double wanted = video ? target : std::max(0.0, target - 0.1);
    id = 0;
    const auto &run = stream.runs.at(range.run);
    const auto &index = video ? run.keys : run.starts;
    auto it = index.upper_bound({wanted + 0.000001, UINT64_MAX});
    if (it == index.begin() || range.end <= target) return false;
    if (std::prev(it)->first.first < range.start - 0.000001) return false;
    id = std::prev(it)->second->id;
    return true;
  }
  Snapshot snapshotLocked(double position) const {
    Snapshot out;
    out.bytes = used_;
    out.transientBytes = transient_bytes_;
    out.capacity = capacity_;
    out.eof = eof_;
    out.backwardBytes = backward_bytes_;
    out.forwardBytes = used_ - backward_bytes_;
    const auto video = currentRange(streams_[0], position);
    const auto audio = currentRange(streams_[1], position);
    out.videoValid = timed_video_ && video.valid;
    out.audioValid = streams_[1].active && audio.valid;
    out.videoEnd = out.videoValid ? video.end : position;
    out.audioEnd = out.audioValid ? audio.end : position;
    const bool valid = (!timed_video_ || video.valid) && (!streams_[1].active || audio.valid);
    out.end = !valid ? position : timed_video_ && streams_[1].active ? std::min(video.end, audio.end)
            : timed_video_ ? video.end : streams_[1].active ? audio.end : position;
    out.end = std::max(position, out.end);
    out.seconds = out.end - position;
    if (config_.enabled) {
      const auto seekVideo = rangeAt(streams_[0], position, true);
      if ((!timed_video_ || seekVideo.valid) && (!streams_[1].active || audio.valid)) {
        out.seekStart = timed_video_ ? std::max(0.0, seekVideo.start) : 0;
        if (streams_[1].active)
          out.seekStart = std::max(out.seekStart, audio.start <= 0 ? 0 : audio.start + 0.1);
        out.seekEnd = timed_video_ && streams_[1].active ? std::min(seekVideo.end, audio.end)
                    : timed_video_ ? seekVideo.end : audio.end;
        if (out.seekStart >= out.seekEnd) out.seekStart = out.seekEnd = 0;
      }
    }
    return out;
  }
  void clearLocked() {
    for (auto &stream : streams_) {
      stream.entries.clear();
      stream.ids.clear();
      stream.runs.clear();
      stream.ends.clear();
      stream.read = stream.entries.end();
      stream.next = stream.cursor = 1;
      stream.lastPopped = stream.run = 0;
      stream.flushed = stream.reliable = false;
      stream.buffered = {};
    }
    used_ = 0;
    transient_bytes_ = 0;
    backward_bytes_ = 0;
    capacity_ = eof_ = transient_ = false;
    refill_needed_ = false;
    refilling_[0] = refilling_[1] = false;
  }

  int64_t cacheLimit() const {
    return config_.network ? config_.forward + config_.backward : config_.forward;
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Stream streams_[2];
  Config config_;
  double refill_from_[2] = {0, 0};
  bool refill_needed_ = false;
  bool refilling_[2] = {false, false};
  double position_ = 0;
  int serial_ = 0;
  int64_t used_ = 0, backward_bytes_ = 0, transient_bytes_ = 0;
  bool timed_video_ = false, aborted_ = false, eof_ = false, capacity_ = false, transient_ = false;
};

} // namespace ffmpeg_player
#endif
