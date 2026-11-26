#pragma once

#include <QMetaObject>
#include <QObject>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace ffmpeg_player {

// Retired sessions and their final decoder references are released here,
// including references returned by the scene graph after the page is gone.
class PlaybackCleanup {
public:
  static PlaybackCleanup &Instance() {
    static PlaybackCleanup cleanup;
    return cleanup;
  }

  void Enqueue(std::function<void()> work) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      work_.push_back(std::move(work));
    }
    changed_.notify_one();
  }

  bool WaitForIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] { return work_.empty() && !busy_; });
  }

private:
  PlaybackCleanup() : worker_([this] { Run(); }) {}
  ~PlaybackCleanup() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
    }
    changed_.notify_one();
    worker_.join();
  }

  void Run() {
    for (;;) {
      std::function<void()> work;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return closing_ || !work_.empty(); });
        if (work_.empty()) return;
        work = std::move(work_.front());
        work_.pop_front();
        busy_ = true;
      }
      work();
      // Dropping the job can enqueue a session's final destruction. Do that
      // before advertising idle, and never while holding the queue mutex.
      work = {};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        if (work_.empty()) idle_.notify_all();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_, idle_;
  std::deque<std::function<void()>> work_;
  bool closing_ = false, busy_ = false;
  std::thread worker_;
};

// Posting and detaching share only this short lock. Workers never own or
// dereference the QML object; queued calls are disabled before its destruction.
class PlaybackUiDispatcher {
  struct Gate {
    std::mutex mutex;
    QObject *receiver = nullptr;
    QMetaObject::Connection destroyedConnection;
    ~Gate() { QObject::disconnect(destroyedConnection); }
  };

public:
  PlaybackUiDispatcher() : gate_(std::make_shared<Gate>()) {}
  explicit PlaybackUiDispatcher(QObject *receiver) : PlaybackUiDispatcher() {
    Attach(receiver);
  }

  void Attach(QObject *receiver) const {
    {
      std::lock_guard<std::mutex> lock(gate_->mutex);
      if (gate_->receiver == receiver) return;
      gate_->receiver = receiver;
    }
    QObject::disconnect(gate_->destroyedConnection);
    gate_->destroyedConnection = {};
    if (receiver) {
      const std::weak_ptr<Gate> weak = gate_;
      gate_->destroyedConnection = QObject::connect(receiver, &QObject::destroyed, receiver, [weak, receiver] {
        if (auto gate = weak.lock()) {
          std::lock_guard<std::mutex> lock(gate->mutex);
          if (gate->receiver == receiver) gate->receiver = nullptr;
        }
      }, Qt::DirectConnection);
    }
  }

  void Disconnect() const {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    gate_->receiver = nullptr;
  }

  void Post(std::function<void()> callback) const {
    const auto gate = gate_;
    std::lock_guard<std::mutex> lock(gate->mutex);
    if (!gate->receiver) return;
    QMetaObject::invokeMethod(gate->receiver, [gate, callback = std::move(callback)] {
      {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->receiver) return;
      }
      callback();
    }, Qt::QueuedConnection);
  }

private:
  std::shared_ptr<Gate> gate_;
};

} // namespace ffmpeg_player
