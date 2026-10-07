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

  size_t Read(uint8_t *data, size_t len) {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_not_empty_.wait(
        lock, [this]() { return data_size_ > 0 || stop_ || flushing_; });

    if (stop_)
      return 0;
    if (flushing_)
      return 0;

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

  // 64 KiB 可缓冲约 0.34 秒的 48 kHz 立体声 S16 音频。
  const size_t RING_BUFFER_SIZE = 64 * 1024;

public:
  AudioDevice(int sample_rate, int channels) {
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
    return ring_buffer_->Write(data, static_cast<size_t>(len), cancel) == static_cast<size_t>(len);
  }

  void Pause(bool pause) {
    if (is_paused_ == pause)
      return;
    is_paused_ = pause;

    if (!pcm_handle)
      return;

    if (can_pause_) {
      std::lock_guard<std::mutex> lock(pcm_mutex_);
      snd_pcm_pause(pcm_handle, pause ? 1 : 0);
    } else {
      // 【硬件不支持暂停时的回退逻辑】
      if (pause) {
        // 不支持暂停时，只能丢弃数据并静音
        Flush();
      }
      // 恢复时不用特意操作，Waiting for data...
    }
  }

  void Flush() {
    if (!ring_buffer_ || !pcm_handle)
      return;

    is_flushing_ = true;
    ring_buffer_->SetFlushing(true);
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    generation_.fetch_add(1);

    // 停止发声
    snd_pcm_drop(pcm_handle);

    ring_buffer_->Reset();
    snd_pcm_prepare(pcm_handle);

    ring_buffer_->SetFlushing(false);
    is_flushing_ = false;
  }

  int GetActualSampleRate() const { return actual_sample_rate; }
  int GetActualChannels() const { return actual_channels; }
  int GetBytesPerFrame() const { return bytes_per_frame; }

  // 获取当前实时延迟（秒）= ALSA 硬件延迟 + RingBuffer 延迟
  double GetCurrentLatencySeconds() {
    if (!pcm_handle)
      return 0;

    std::lock_guard<std::mutex> lock(pcm_mutex_);
    snd_pcm_sframes_t delay_frames;
    // 获取 ALSA 硬件缓冲区里的延迟帧数
    if (snd_pcm_delay(pcm_handle, &delay_frames) < 0) {
      delay_frames = 0;
    }

    // 加上 RingBuffer 里的数据量
    size_t ring_buffer_bytes = ring_buffer_->GetDataSize();
    double ring_buffer_delay =
        (double)ring_buffer_bytes / bytes_per_frame / actual_sample_rate;

    double alsa_delay = (double)delay_frames / actual_sample_rate;

    return std::max(0.0, alsa_delay) + ring_buffer_delay;
  }

private:
  bool Recover(int err) {
    if (err == -EPIPE) {
      // XRUN (Underrun)
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
      if (is_paused_ || is_flushing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      // 核心改动：Read 应该是阻塞的（由 RingBuffer 保证），一旦有数据就处理
      const uint64_t generation = generation_.load();
      size_t bytes_read = ring_buffer_->Read(read_buf.data(), chunk_bytes);
      if (bytes_read == 0)
        continue;

      int frames_to_write = bytes_read / bytes_per_frame;
      uint8_t *ptr = read_buf.data();

      while (frames_to_write > 0 && running_ && !is_flushing_) {
        // Preserve a partially written period across hardware pause/resume.
        // Flush invalidates it through generation_, including pause fallback.
        if (generation != generation_.load())
          break;
        if (is_paused_) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        if (generation != generation_.load())
          break;
        if (is_paused_)
          continue;
        snd_pcm_sframes_t written =
            snd_pcm_writei(pcm_handle, ptr, frames_to_write);

        if (written < 0) {
          if (written == -EAGAIN) {
            // 此时硬件缓冲区满了，稍微等一下
            snd_pcm_wait(pcm_handle, 5);
            continue;
          }
          if (Recover((int)written))
            continue;
          break;
        }
        if (written == 0) {
          snd_pcm_wait(pcm_handle, 5);
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
