#pragma once

#include "RimeApiWrapper.h"
#include <cstdint>

namespace mod::rime {

// GUI callers must check available() before touching a session. Maintenance is
// reserved on the GUI thread before its worker starts, and released only after
// that worker has joined librime's maintenance thread. The generation also
// prevents an old Session* ID from accidentally referring to a new owner.
class RimeRuntime {
public:
    static RimeApi* api();
    static bool     available();
    static uint64_t generation();
    static bool     beginMaintenance();
    static void     endMaintenance();
    static void     notification(const char* type, const char* value);
    static bool     maintenanceSucceeded();
};

} // namespace mod::rime
