#include "RimeRuntime.h"

#include <atomic>
#include <cstring>

namespace mod::rime {
namespace {
std::atomic<bool>     maintenancePending{false};
std::atomic<uint64_t> sessionGeneration{1};
std::atomic<int>      maintenanceResult{0};
} // namespace

RimeApi* RimeRuntime::api() {
    static RimeApi* api = rime_get_api();
    return api;
}

bool RimeRuntime::available() {
    if (maintenancePending.load()) return false;
    auto* api = RimeRuntime::api();
    return api && (!api->is_maintenance_mode || !api->is_maintenance_mode());
}

uint64_t RimeRuntime::generation() { return sessionGeneration.load(); }

bool RimeRuntime::beginMaintenance() {
    if (!available() || maintenancePending.exchange(true)) return false;
    maintenanceResult.store(0);
    sessionGeneration.fetch_add(1);
    return true;
}

void RimeRuntime::endMaintenance() { maintenancePending.store(false); }

void RimeRuntime::notification(const char* type, const char* value) {
    if (!maintenancePending.load() || !type || !value || std::strcmp(type, "deploy") != 0) return;
    if (std::strcmp(value, "success") == 0) maintenanceResult.store(1);
    else if (std::strcmp(value, "failure") == 0) maintenanceResult.store(-1);
}

bool RimeRuntime::maintenanceSucceeded() { return maintenanceResult.load() == 1; }

} // namespace mod::rime
