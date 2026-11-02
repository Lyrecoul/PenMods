#ifndef AUDIO_DEVICE_HPP
#define AUDIO_DEVICE_HPP

#include "Logger.hpp"
#include <algorithm>
#include <alsa/asoundlib.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <vector>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ffmpeg_player {

class RingBuffer {
public:
  RingBuffer(size_t size)
      : buffer_(size), size_(size), read_pos_(0), write_pos_(0), data_size_(0) {
  }

  size_t Write(const uint8_t *data, size_t len,
               const std::function<bool()> &cancel = {}) {
    std::unique_lock<std::mutex> lock(mutex_);
    size_t written = 0;
    while (written < len) {
      if (flushing_)
        return written;
      // 收到 stop_ 后立即停止等待
      cond_not_full_.wait_for(lock, std::chrono::milliseconds(10),
          [this]() { return data_size_ < size_ || stop_ || flushing_; });
      if (cancel && cancel())
        return written;
      if (stop_ || flushing_)
        return written;

      size_t available = size_ - data_size_;
      size_t chunk = std::min(len - written, available);
      size_t first_chunk = std::min(chunk, size_ - write_pos_);
      memcpy(buffer_.data() + write_pos_, data + written, first_chunk);
      memcpy(buffer_.data(), data + written + first_chunk, chunk - first_chunk);

      write_pos_ = (write_pos_ + chunk) % size_;
      data_size_ += chunk;
      written += chunk;
      cond_not_empty_.notify_one();
    }
    return written;
  }

  size_t Read(uint8_t *data, size_t len, uint64_t &generation,
              const std::atomic<uint64_t> &current_generation) {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_not_empty_.wait_for(lock, std::chrono::milliseconds(5),
        [this]() { return data_size_ > 0 || stop_ || flushing_; });

    if (stop_)
      return 0;
    if (flushing_)
      return 0;

    // 等待数据期间可能已经 Flush；代次必须与实际读出的 PCM 一起取得。
    generation = current_generation.load(std::memory_order_acquire);
    size_t chunk = std::min(len, data_size_);
    size_t first_chunk = std::min(chunk, size_ - read_pos_);
    memcpy(data, buffer_.data() + read_pos_, first_chunk);
    memcpy(data + first_chunk, buffer_.data(), chunk - first_chunk);

    read_pos_ = (read_pos_ + chunk) % size_;
    data_size_ -= chunk;
    cond_not_full_.notify_one();
    return chunk;
  }

  size_t GetDataSize() {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_size_;
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    read_pos_ = 0;
    write_pos_ = 0;
    data_size_ = 0;
    cond_not_full_.notify_all();
  }

  void SetFlushing(bool flush) {
    std::lock_guard<std::mutex> lock(mutex_);
    flushing_ = flush;
    cond_not_full_.notify_all();
    cond_not_empty_.notify_all();
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    // 唤醒所有等待的读写线程
    cond_not_empty_.notify_all();
    cond_not_full_.notify_all();
  }

private:
  std::vector<uint8_t> buffer_;
  size_t size_;
  size_t read_pos_;
  size_t write_pos_;
  size_t data_size_;
  bool stop_ = false;
  bool flushing_ = false;
  std::mutex mutex_;
  std::condition_variable cond_not_full_;
  std::condition_variable cond_not_empty_;
};

class AudioDevice {
private:
  // 词典笔音频服务通过此目录内的 PID 锁决定是否开放输出。
  // 每个输出实例独立持有文件，避免一个播放器关闭另一个的锁。
  class WakeLock {
  public:
    ~WakeLock() { Release(); }
    bool Acquire() {
      const char *configured = std::getenv("FFPLAYER_AUDIO_LOCK_DIR");
      const std::string dir = configured ? configured : "/tmp/audio_wakelocks";
      struct stat info;
      if (stat(dir.c_str(), &info) < 0) {
        if (!configured && errno == ENOENT)
          return true; // 普通 Linux 主机没有词典笔音频服务。
        return false;
      }
      if (!S_ISDIR(info.st_mode))
        return false;
      std::string pattern = dir + "/ffmpegplayer-" + std::to_string(getpid()) + "-XXXXXX.lock";
      std::vector<char> name(pattern.begin(), pattern.end());
      name.push_back('\0');
      const int fd = mkstemps(name.data(), 5);
      if (fd < 0)
        return false;
      path_ = name.data();
      const std::string pid = std::to_string(getpid()) + "\n";
      ssize_t written;
      do {
        written = write(fd, pid.data(), pid.size());
      } while (written < 0 && errno == EINTR);
      const int closed = close(fd);
      if (written != static_cast<ssize_t>(pid.size()) || closed < 0) {
        Release();
        return false;
      }
      return true;
    }
    void Release() {
      if (!path_.empty()) {
        unlink(path_.c_str());
        path_.clear();
      }
    }
  private:
    std::string path_;
  } wake_lock_;

  snd_pcm_t *pcm_handle = nullptr;
  size_t bytes_per_frame;
  int actual_sample_rate;
  int actual_channels;

  bool can_pause_ = false;
  snd_pcm_uframes_t buffer_size_frames_ = 0;
  snd_pcm_uframes_t period_size_frames_ = 0;

  std::unique_ptr<RingBuffer> ring_buffer_;
  std::thread audio_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> is_paused_{false};
  std::atomic<bool> is_flushing_{false};
  std::mutex pcm_mutex_;
  std::atomic<uint64_t> generation_{0};
  std::atomic<uint64_t> fed_frames_{0};
  uint64_t released_frames_ = 0; // 已写入 ALSA 或因错误丢弃；pcm_mutex_ 保护
  uint64_t underruns_ = 0, dropped_frames_ = 0, flushes_ = 0;
  std::function<bool()> pause_requested_;
  std::function<void(bool, double)> pause_confirmed_;
  bool pause_applied_ = false, hardware_paused_ = false;
  std::vector<uint8_t> submitted_, replay_;

  // 64 KiB 可缓冲约 0.34 秒的 48 kHz 立体声 S16 音频。
  const size_t RING_BUFFER_SIZE = 64 * 1024;

public:
  AudioDevice(int sample_rate, int channels,
              std::function<bool()> pauseRequested = {},
              std::function<void(bool, double)> pauseConfirmed = {})
      : pause_requested_(std::move(pauseRequested)), pause_confirmed_(std::move(pauseConfirmed)) {
    actual_sample_rate = sample_rate;
    actual_channels = channels;
    bytes_per_frame = channels * 2;

    if (sample_rate <= 0 || channels <= 0)
      return;

    ring_buffer_ = std::make_unique<RingBuffer>(RING_BUFFER_SIZE);

    if (!wake_lock_.Acquire()) {
      LOG(ERROR) << "Cannot acquire audio wake lock";
      return;
    }
    if (InitALSA(sample_rate, channels) < 0) {
      LOG(ERROR) << "Failed to initialize ALSA";
      wake_lock_.Release();
      return;
    }

    const size_t retained = (buffer_size_frames_ + period_size_frames_) * bytes_per_frame;
    submitted_.reserve(retained);
    replay_.reserve(retained);
    running_ = true;
    audio_thread_ = std::thread(&AudioDevice::AudioLoop, this);
  }

  ~AudioDevice() {
    // 1. 设置标志位
    running_ = false;

    // 2. 唤醒 RingBuffer 的所有等待者 (包括 Feed 函数和 Loop 中的 Read)
    if (ring_buffer_)
      ring_buffer_->Stop();

    // ALSA 使用非阻塞写入，先等待线程退出，避免并发访问 PCM 句柄。
    if (audio_thread_.joinable()) {
      audio_thread_.join();
    }

    if (pcm_handle) {
      snd_pcm_drop(pcm_handle);
      snd_pcm_close(pcm_handle);
    }
  }

  int InitALSA(int sample_rate, int channels) {
    int err;
    const char *device_name = "default";

    if ((err = snd_pcm_open(&pcm_handle, device_name, SND_PCM_STREAM_PLAYBACK,
                            SND_PCM_NONBLOCK)) < 0) {
      LOG(ERROR) << "ALSA Open Error: " << snd_strerror(err);
      return err;
    }

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    if ((err = snd_pcm_hw_params_any(pcm_handle, hw_params)) < 0)
      return err;

    // 1. 强制使用硬件最喜欢的访问模式
    if ((err = snd_pcm_hw_params_set_access(pcm_handle, hw_params,
                                            SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
      return err;

    // 2. 使用 S16_LE 减少内存带宽占用（RK3326 DDR 带宽有限）
    // 虽然 S32_LE 是原生格式，但 S16_LE 数据量减半，对带宽更友好
    if ((err = snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0)
      return err;
    bytes_per_frame = channels * 2;
    LOG(INFO) << "ALSA using S16_LE (bandwidth optimized)";

    if ((err = snd_pcm_hw_params_set_channels(pcm_handle, hw_params, channels)) < 0)
      return err;

    // 接受设备支持的采样率，并交给前级滤镜完成重采样。
    unsigned int rate = static_cast<unsigned int>(sample_rate);
    if ((err = snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &rate, 0)) < 0)
      return err;
    actual_sample_rate = rate; // 把这个值传回给前面的 Resampler

    // 请求 200 ms 的硬件缓冲区与 20 ms 的 period。
    unsigned int buffer_time = 200000;
    unsigned int period_time = 20000;
    snd_pcm_hw_params_set_buffer_time_near(pcm_handle, hw_params, &buffer_time,
                                           0);
    snd_pcm_hw_params_set_period_time_near(pcm_handle, hw_params, &period_time,
                                           0);

    if ((err = snd_pcm_hw_params(pcm_handle, hw_params)) < 0) {
      LOG(ERROR) << "ALSA Set HW Params Error: " << snd_strerror(err);
      return err;
    }

    // 获取最终确定的参数
    if ((err = snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size_frames_)) < 0)
      return err;
    if ((err = snd_pcm_hw_params_get_period_size(hw_params, &period_size_frames_, 0)) < 0)
      return err;
    if (period_size_frames_ == 0)
      return -EINVAL;
    can_pause_ = snd_pcm_hw_params_can_pause(hw_params);

    LOG(INFO) << "ALSA Initialized: Buffer=" << buffer_size_frames_
              << " frames, Period=" << period_size_frames_
              << " frames, Hardware Pause=" << (can_pause_ ? "Yes" : "No");

    // 5. 软件参数设置：减少 Xrun 发生
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_sw_params_alloca(&sw_params);
    if ((err = snd_pcm_sw_params_current(pcm_handle, sw_params)) < 0)
      return err;
    // 缓冲区剩 1/4 时就唤醒写入
    if ((err = snd_pcm_sw_params_set_avail_min(pcm_handle, sw_params, period_size_frames_)) < 0)
      return err;
    // 立即启动
    if ((err = snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, 1)) < 0)
      return err;
    if ((err = snd_pcm_sw_params(pcm_handle, sw_params)) < 0)
      return err;

    return 0;
  }

  bool IsValid() const { return running_.load(); }

  bool Feed(uint8_t *data, int len, const std::function<bool()> &cancel = {}) {
    if (!ring_buffer_ || is_flushing_ || !running_ || len < 0)
      return false;
    const size_t fed = ring_buffer_->Write(data, static_cast<size_t>(len), cancel);
    fed_frames_.fetch_add(fed / bytes_per_frame, std::memory_order_relaxed);
    return fed == static_cast<size_t>(len);
  }

  void Pause(bool pause) {
    is_paused_ = pause;
  }

  void Flush() {
    if (!ring_buffer_ || !pcm_handle)
      return;

    is_flushing_ = true;
    ring_buffer_->SetFlushing(true);
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    generation_.fetch_add(1);
    ++flushes_;

    // 停止发声
    snd_pcm_drop(pcm_handle);

    ring_buffer_->Reset();
    fed_frames_.store(0, std::memory_order_relaxed);
    released_frames_ = 0;
    submitted_.clear();
    replay_.clear();
    pause_applied_ = hardware_paused_ = false;
    snd_pcm_prepare(pcm_handle);

    ring_buffer_->SetFlushing(false);
    is_flushing_ = false;
  }

  int GetActualSampleRate() const { return actual_sample_rate; }
  int GetActualChannels() const { return actual_channels; }
  int GetBytesPerFrame() const { return bytes_per_frame; }

  struct PlaybackStats {
    double latency = 0;
    uint64_t underruns = 0, dropped = 0, flushes = 0;
  };

  // 包括已从 RingBuffer 取出、尚未写入 ALSA 的一个 period。
  PlaybackStats GetPlaybackStats() {
    if (!pcm_handle)
      return {};

    std::lock_guard<std::mutex> lock(pcm_mutex_);
    snd_pcm_sframes_t delay_frames;
    // 获取 ALSA 硬件缓冲区里的延迟帧数
    if (snd_pcm_delay(pcm_handle, &delay_frames) < 0) {
      delay_frames = 0;
    }

    const uint64_t fed = fed_frames_.load(std::memory_order_relaxed);
    const uint64_t pending = fed > released_frames_ ? fed - released_frames_ : 0;
    return {(std::max<snd_pcm_sframes_t>(0, delay_frames) + static_cast<double>(pending)) /
                actual_sample_rate, underruns_, dropped_frames_, flushes_};
  }

  double GetCurrentLatencySeconds() { return GetPlaybackStats().latency; }

private:
  void TrimSubmitted() {
    snd_pcm_sframes_t delay = 0;
    // On a failed query retain the bounded copy: failure is not evidence that
    // samples played. XRUN confirms the device queue has drained.
    if (snd_pcm_delay(pcm_handle, &delay) < 0) {
      if (snd_pcm_state(pcm_handle) != SND_PCM_STATE_XRUN) return;
      delay = 0;
    }
    const size_t keep = std::min(submitted_.size(),
        static_cast<size_t>(std::max<snd_pcm_sframes_t>(0, delay)) * bytes_per_frame);
    submitted_.erase(submitted_.begin(), submitted_.end() - keep);
  }

  void ApplyPause() {
    const bool requested = pause_requested_ ? pause_requested_() : is_paused_.load();
    is_paused_ = requested;
    std::unique_lock<std::mutex> lock(pcm_mutex_);
    if (requested == pause_applied_ || is_flushing_) return;
    if (requested) {
      hardware_paused_ = can_pause_ && snd_pcm_pause(pcm_handle, 1) >= 0;
      TrimSubmitted();
      if (!hardware_paused_) {
        // Withdraw the safe tail atomically at the ALSA application pointer.
        // A delay snapshot followed by drop alone races device consumption.
        const auto safe = snd_pcm_rewindable(pcm_handle);
        const auto wanted = std::min<size_t>(submitted_.size() / bytes_per_frame,
            std::max<snd_pcm_sframes_t>(0, safe));
        const auto rewound = wanted ? snd_pcm_rewind(pcm_handle, wanted) : 0;
        if (rewound > 0) {
          const size_t bytes = std::min<size_t>(rewound * bytes_per_frame, submitted_.size());
          replay_.insert(replay_.begin(), submitted_.end() - bytes, submitted_.end());
          released_frames_ -= bytes / bytes_per_frame;
          submitted_.resize(submitted_.size() - bytes);
        }
        TrimSubmitted();
        // Some plugins cannot rewind their downstream hardware. Let that
        // bounded residual finish; never guess which of those samples played.
        if (!submitted_.empty()) return;
        snd_pcm_drop(pcm_handle);
        snd_pcm_prepare(pcm_handle);
      }
    } else if (hardware_paused_) {
      snd_pcm_pause(pcm_handle, 0);
      hardware_paused_ = false;
    }
    pause_applied_ = requested;
    lock.unlock();
    if (pause_confirmed_) pause_confirmed_(requested, GetCurrentLatencySeconds());
  }

  // Caller holds pcm_mutex_. The allocation is bounded by the ALSA buffer plus
  // one period, regardless of playback duration.
  snd_pcm_sframes_t Write(const uint8_t *data, size_t frames) {
    TrimSubmitted();
    const size_t limit = (buffer_size_frames_ + period_size_frames_) * bytes_per_frame;
    frames = std::min(frames, (limit - submitted_.size()) / bytes_per_frame);
    if (!frames) return -EAGAIN;
    const auto written = snd_pcm_writei(pcm_handle, data, frames);
    if (written > 0) submitted_.insert(submitted_.end(), data, data + written * bytes_per_frame);
    return written;
  }

  bool Recover(int err) {
    if (err == -EPIPE) {
      // XRUN (Underrun)
      ++underruns_;
      if ((err = snd_pcm_prepare(pcm_handle)) < 0)
        return false;
      return true;
    } else if (err == -ESTRPIPE) {
      // Suspend
      while (running_ && (err = snd_pcm_resume(pcm_handle)) == -EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (err < 0)
        snd_pcm_prepare(pcm_handle);
      return true;
    }
    return false;
  }

  void AudioLoop() {
    // 1. 提升线程优先级为实时调度（最高优先级）
    struct sched_param param;
    param.sched_priority = 99; // 最高实时优先级
    pthread_setschedparam(pthread_self(), SCHED_RR, &param);

    // 2. 绑核：把音频输出锁在 CPU 3 上（最后一个核）
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    LOG(INFO) << "AudioLoop: priority=99, bound to CPU 3";

    // 每次读取一个 period 的量
    size_t chunk_bytes = period_size_frames_ * bytes_per_frame;
    std::vector<uint8_t> read_buf(chunk_bytes);

    while (running_) {
      ApplyPause();
      if (is_paused_ || is_flushing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      {
        std::unique_lock<std::mutex> lock(pcm_mutex_);
        if (!replay_.empty()) {
          const auto written = Write(replay_.data(),
              std::min<size_t>(period_size_frames_, replay_.size() / bytes_per_frame));
          if (written > 0) {
            released_frames_ += written;
            replay_.erase(replay_.begin(), replay_.begin() + written * bytes_per_frame);
          } else if (written < 0 && written != -EAGAIN) Recover(static_cast<int>(written));
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
      }

      // 核心改动：Read 应该是阻塞的（由 RingBuffer 保证），一旦有数据就处理
      uint64_t generation = 0;
      size_t bytes_read = ring_buffer_->Read(read_buf.data(), chunk_bytes, generation, generation_);
      if (bytes_read == 0)
        continue;

      int frames_to_write = bytes_read / bytes_per_frame;
      uint8_t *ptr = read_buf.data();

      while (frames_to_write > 0 && running_ && !is_flushing_) {
        ApplyPause();
        // Preserve a partially written period across hardware pause/resume.
        // Flush invalidates it through generation_, including pause fallback.
        if (generation != generation_.load())
          break;
        if (is_paused_) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        std::unique_lock<std::mutex> lock(pcm_mutex_);
        if (generation != generation_.load())
          break;
        if (is_paused_)
          continue;
#ifdef FFPLAYER_VALIDATION
        // A null ALSA sink accepts samples instantly. Simulate device time in
        // validation builds so decoder throughput is compared to a real clock.
        if (std::getenv("FFPLAYER_TEST_PACED_AUDIO")) {
          lock.unlock();
          double remaining = static_cast<double>(frames_to_write) / actual_sample_rate;
          auto previous = std::chrono::steady_clock::now();
          while (remaining > 0 && running_ && !is_flushing_ && generation == generation_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto now = std::chrono::steady_clock::now();
            if (!is_paused_) remaining -= std::chrono::duration<double>(now - previous).count();
            previous = now;
          }
          lock.lock();
          if (!running_ || is_flushing_ || generation != generation_.load()) break;
        }
#endif
        const bool replaying = !replay_.empty();
        snd_pcm_sframes_t written = replaying
            ? Write(replay_.data(), std::min<size_t>(period_size_frames_, replay_.size() / bytes_per_frame))
            : Write(ptr, frames_to_write);

        if (written < 0) {
          if (written == -EAGAIN) {
            // Waiting with pcm_mutex_ held starves latency/clock updates on
            // the lower-priority feeder thread. No PCM call runs unlocked.
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
          }
          if (Recover((int)written))
            continue;
          // 丢弃的样本也要从待播计数中移除，避免 EOF 永远等待这段延迟。
          released_frames_ += static_cast<uint64_t>(frames_to_write);
          dropped_frames_ += static_cast<uint64_t>(frames_to_write);
          LOG(ERROR) << "ALSA write failed: " << snd_strerror(static_cast<int>(written));
          break;
        }
        if (written == 0) {
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
        released_frames_ += static_cast<uint64_t>(written);
        if (replaying) {
          replay_.erase(replay_.begin(), replay_.begin() + written * bytes_per_frame);
          continue;
        }
        ptr += written * bytes_per_frame;
        frames_to_write -= written;
      }
    }
  }
};

} // namespace ffmpeg_player
#endif
