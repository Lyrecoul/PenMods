#ifndef FRAME_QUEUE_HPP
#define FRAME_QUEUE_HPP

#include "FFmpegCore.hpp"
#include <condition_variable>
#include <mutex>
#include <queue>

namespace ffmpeg_player {

class FrameQueue {
private:
  struct Entry { AVFrame *frame; int serial; uint64_t generation; };
  std::queue<Entry> queue;
  std::mutex mtx;
  std::condition_variable cond;
  size_t max_size = 10; // 默认最大缓存 10 个帧
  bool abort_request = false;

public:
  FrameQueue(size_t capacity = 10) : max_size(capacity) {}

  ~FrameQueue() { Clear(); }

  // 入队
  inline bool Push(AVFrame *frame) {
    return Push(frame, -1); // 默认无超时，保持向后兼容
  }

  // 入队（带超时）
  // timeout_ms: 超时时间（毫秒），-1 表示无限等待
  inline bool Push(AVFrame *frame, int timeout_ms, int serial = 0, uint64_t generation = 0) {
    std::unique_lock<std::mutex> lock(mtx);

    if (timeout_ms < 0) {
        // 无限等待
        cond.wait(lock, [this] { return queue.size() < max_size || abort_request; });
    } else {
        // 有限等待
        bool success = cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                     [this] { return queue.size() < max_size || abort_request; });
        if (!success) return false; // 超时返回
    }

    if (abort_request)
      return false;

    // 复制帧数据以确保安全
    AVFrame *copy_frame = av_frame_alloc();
    if (!copy_frame) return false;

    if (av_frame_ref(copy_frame, frame) < 0) {
      av_frame_free(&copy_frame);
      return false;
    }
    queue.push({copy_frame, serial, generation});
    cond.notify_one();
    return true;
  }

  // 出队
  // block: 是否阻塞等待
  inline bool Pop(AVFrame *&frame, bool block = true) {
    return Pop(frame, block, -1); // 默认无超时，保持向后兼容
  }

  // 出队（带超时）
  // timeout_ms: 超时时间（毫秒），-1 表示无限等待
  inline bool Pop(AVFrame *&frame, bool block, int timeout_ms, int *serial = nullptr,
                  uint64_t *generation = nullptr) {
    std::unique_lock<std::mutex> lock(mtx);

    if (block && timeout_ms < 0) {
      // 无限等待
      cond.wait(lock, [this] { return !queue.empty() || abort_request; });
    } else if (block && timeout_ms >= 0) {
      // 有限等待
      bool success =
          cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return !queue.empty() || abort_request; });
      if (!success)
        return false; // 超时
    }

    if (abort_request || queue.empty())
      return false;

    frame = queue.front().frame;
    if (serial)
      *serial = queue.front().serial;
    if (generation)
      *generation = queue.front().generation;
    queue.pop();

    cond.notify_one(); // 通知 Push 线程队列有空间了
    return true;
  }

  // 清空队列（用于 Seek 或者停止播放）
  inline void Clear() {
    std::lock_guard<std::mutex> lock(mtx);
    while (!queue.empty()) {
      AVFrame *frame = queue.front().frame;
      queue.pop();
      av_frame_free(&frame); // 必须彻底释放内存
    }
    cond.notify_all();
  }

  inline void Abort() {
    std::lock_guard<std::mutex> lock(mtx);
    abort_request = true;
    cond.notify_all();
  }

  // Abort 之后重新开始播放前调用
  inline void Resume() {
    std::lock_guard<std::mutex> lock(mtx);
    abort_request = false;
  }

  inline size_t Size() {
    std::lock_guard<std::mutex> lock(mtx);
    return queue.size();
  }
};

} // namespace ffmpeg_player
#endif
