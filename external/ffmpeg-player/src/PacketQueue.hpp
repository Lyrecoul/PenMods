#ifndef PACKET_QUEUE_HPP
#define PACKET_QUEUE_HPP

#include "FFmpegCore.hpp"
#include <condition_variable>
#include <mutex>
#include <queue>

namespace ffmpeg_player {

class PacketQueue {
private:
  std::queue<std::pair<AVPacket *, int>> queue;
  std::mutex mtx;
  std::condition_variable cond;
  size_t max_size = 100; // 默认最大缓存 100 个包
  bool abort_request = false;

public:
  PacketQueue(size_t capacity = 100) : max_size(capacity) {}

  ~PacketQueue() { Clear(); }

  // 入队
  inline bool Push(AVPacket *pkt) {
    return Push(pkt, -1); // 默认无超时，保持向后兼容
  }

  // 入队（带超时）
  // timeout_ms: 超时时间（毫秒），-1 表示无限等待
  inline bool Push(AVPacket *pkt, int timeout_ms, int serial = 0) {
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

    // 注意：我们直接存入指针，所有权转移到队列
    queue.push({pkt, serial});
    cond.notify_one();
    return true;
  }

  // 出队
  // block: 是否阻塞等待
  inline bool Pop(AVPacket *&pkt, bool block = true) {
    return Pop(pkt, block, -1); // 默认无超时，保持向后兼容
  }

  // 出队（带超时）
  // timeout_ms: 超时时间（毫秒），-1 表示无限等待
  inline bool Pop(AVPacket *&pkt, bool block, int timeout_ms, int *serial = nullptr) {
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

    pkt = queue.front().first;
    if (serial)
      *serial = queue.front().second;
    queue.pop();

    cond.notify_one(); // 通知 Push 线程队列有空间了
    return true;
  }

  // 清空队列（用于 Seek 或者停止播放）
  inline void Clear() {
    std::lock_guard<std::mutex> lock(mtx);
    while (!queue.empty()) {
      AVPacket *pkt = queue.front().first;
      queue.pop();
      av_packet_free(&pkt); // 必须彻底释放内存
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
