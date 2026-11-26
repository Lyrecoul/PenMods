#pragma once

#include "Logger.hpp"
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace ffmpeg_player {

// Opt-in because logging itself can affect timing on the device. All stages
// share a monotonic timestamp and thread id, including the cleanup worker.
class ShutdownTrace {
public:
  static bool Enabled() {
#ifdef FFPLAYER_SHUTDOWN_DIAGNOSTICS
    return true;
#else
    static const bool enabled = [] {
      const char *value = std::getenv("FFPLAYER_SHUTDOWN_TRACE");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
#endif
  }

  static double Now() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  static void Event(const char *stage, const void *owner, double since,
                    const char *phase = "event") {
    if (!Enabled()) return;
    const double now = Now();
    std::ostringstream message;
    message << "SHUTDOWN stage=" << stage << " phase=" << phase << " object=" << owner
            << " thread=" << std::this_thread::get_id()
            << " time_ms=" << static_cast<int64_t>(now)
            << " elapsed_ms=" << now - since;
    Write(message.str());
  }

  static void Write(const std::string &message) {
    LOG(INFO) << message;
#ifdef FFPLAYER_SHUTDOWN_DIAGNOSTICS
    // The device launcher may discard stderr. Diagnostic packages also leave
    // a small, directly retrievable log, without changing the shared logger.
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static std::ofstream output("/tmp/ffplayer-shutdown.log", std::ios::app);
    output << message << '\n';
    output.flush();
#endif
  }

  ShutdownTrace(const char *stage, const void *owner, bool active = true)
      : stage_(stage), owner_(owner), active_(active && Enabled()),
        start_(active_ ? Now() : 0) {
    if (active_) Event(stage_, owner_, start_, "begin");
  }
  ~ShutdownTrace() {
    if (active_) Event(stage_, owner_, start_, "end");
  }
  void Step(const char *stage) const {
    if (active_) Event(stage, owner_, start_);
  }

private:
  const char *stage_;
  const void *owner_;
  bool active_;
  double start_;
};

} // namespace ffmpeg_player
