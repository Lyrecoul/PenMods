#pragma once

#include "ShutdownTrace.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QThread>
#include <QTimer>
#include <array>

namespace ffmpeg_player {

// A short-lived application filter, used only by diagnostic builds. Copy event
// metadata before delivery: a receiver may be deleted by that very event.
// No per-event I/O and no retained pointers to receivers or their parents.
class ShutdownEventProbe final : public QObject {
public:
  explicit ShutdownEventProbe(QObject *parent) : QObject(parent) {
    QCoreApplication::instance()->installEventFilter(this);
    ShutdownTrace::Event("event-probe-v2", this, ShutdownTrace::Now());
  }

protected:
  bool eventFilter(QObject *receiver, QEvent *event) override {
    if (QThread::currentThread() != thread()) return false;
    const double now = ShutdownTrace::Now();
    if (count_ && now - events_[(count_ - 1) % events_.size()].time >= 100 && reports_ < 4) {
      ++reports_;
      const auto &last = events_[(count_ - 1) % events_.size()];
      ShutdownTrace::Event("gui-event-gap", this, last.time);
      const size_t begin = count_ > events_.size() ? count_ - events_.size() : 0;
      for (size_t i = begin; i < count_; ++i) {
        const auto &entry = events_[i % events_.size()];
        std::ostringstream line;
        line << "SHUTDOWN_EVENT time_ms=" << static_cast<int64_t>(entry.time)
             << " receiver=0x" << std::hex << entry.address << std::dec
             << " type=" << entry.type << " timer_id=" << entry.timerId
             << " interval_ms=" << entry.interval << " ancestry=" << entry.ancestry.constData();
        ShutdownTrace::Write(line.str());
      }
    }
    auto &entry = events_[count_++ % events_.size()];
    entry.time = now;
    entry.address = reinterpret_cast<uintptr_t>(receiver);
    entry.type = static_cast<int>(event->type());
    entry.timerId = event->type() == QEvent::Timer ? static_cast<QTimerEvent *>(event)->timerId() : -1;
    const auto *timer = qobject_cast<QTimer *>(receiver);
    entry.interval = timer ? timer->interval() : -1;
    entry.ancestry.clear();
    auto *object = receiver;
    for (int depth = 0; object && depth < 4; ++depth, object = object->parent()) {
      if (depth) entry.ancestry += " <- ";
      entry.ancestry += object->metaObject()->className();
    }
    return false;
  }

private:
  struct Entry {
    double time = 0;
    uintptr_t address = 0;
    int type = 0, timerId = -1, interval = -1;
    QByteArray ancestry;
  };
  std::array<Entry, 16> events_;
  size_t count_ = 0;
  unsigned reports_ = 0;
};

} // namespace ffmpeg_player
