#include "common/Event.h"
#include "rime/Backend.h"
#include "rime/RimeRuntime.h"
#include "rime/RimeWrapper.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

std::atomic<bool> mod::Logger::s_configLoaded{false};
void              mod::Logger::_applyLevelFromConfig(const std::string&) {}
mod::Event::Event() = default;

namespace {
RimeApi                              api{};
const auto                           guiThread = std::this_thread::get_id();
std::mutex                           mutex;
std::condition_variable              changed;
std::map<RimeSessionId, std::string> sessions;
RimeSessionId                        nextId = 1;
std::atomic<int>                     creates{0}, sessionCalls{0}, threadViolations{0};
std::atomic<bool>                    nativeBusy{false}, joinEntered{false};
bool                                 finish           = false;
bool                                 operationSuccess = true;
int                                  statusBorrows    = 0;
int                                  lastFullCheck    = -1;
int                                  failures         = 0;
std::string                          schema(400, 'x');
void                                 check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
void guiSessionCall() {
    ++sessionCalls;
    if (std::this_thread::get_id() != guiThread || nativeBusy.load()) ++threadViolations;
}
RimeSessionId create() {
    guiSessionCall();
    ++creates;
    const auto id = nextId++;
    sessions[id]  = "";
    return id;
}
int find(RimeSessionId id) {
    guiSessionCall();
    return sessions.count(id);
}
int destroy(RimeSessionId id) {
    guiSessionCall();
    return sessions.erase(id) != 0;
}
void cleanup() {
    if (std::this_thread::get_id() == guiThread) ++threadViolations;
    sessions.clear();
    nextId = 1; // Deliberately reuse pointer-like session IDs.
}
int process(RimeSessionId id, int key, int) {
    guiSessionCall();
    sessions.at(id) += static_cast<char>(key);
    return 1;
}
void clear(RimeSessionId id) {
    guiSessionCall();
    sessions.at(id).clear();
}
int context(RimeSessionId id, RimeContext* ctx) {
    guiSessionCall();
    ctx->composition.preedit = const_cast<char*>(sessions.at(id).c_str());
    return 1;
}
int freeContext(RimeContext*) { return 1; }
int status(RimeSessionId, RimeStatus* out) {
    guiSessionCall();
    ++statusBorrows;
    out->schema_id = strdup(schema.c_str());
    return 1;
}
int freeStatus(RimeStatus* out) {
    --statusBorrows;
    free(out->schema_id);
    return 1;
}
int select(RimeSessionId, const char* value) {
    guiSessionCall();
    schema = value;
    return 1;
}
int start(int full) {
    if (std::this_thread::get_id() == guiThread) ++threadViolations;
    lastFullCheck = full;
    nativeBusy    = true;
    return 1;
}
int sync() {
    cleanup();
    return start(-1);
}
void join() {
    if (std::this_thread::get_id() == guiThread) ++threadViolations;
    std::unique_lock<std::mutex> lock(mutex);
    joinEntered = true;
    changed.wait(lock, [] { return finish; });
    mod::rime::RimeRuntime::notification("deploy", operationSuccess ? "success" : "failure");
    nativeBusy = false;
}
bool waitUntil(const std::function<bool()>& predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}
void prepare(bool success) {
    std::lock_guard<std::mutex> lock(mutex);
    finish           = false;
    operationSuccess = success;
    joinEntered      = false;
}
void release() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        finish = true;
    }
    changed.notify_all();
}
} // namespace
extern "C" RimeApi* rime_get_api() { return &api; }
int                 main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    RIME_STRUCT_INIT(RimeApi, api);
    api.create_session          = create;
    api.find_session            = find;
    api.destroy_session         = destroy;
    api.cleanup_all_sessions    = cleanup;
    api.process_key             = process;
    api.clear_composition       = clear;
    api.get_context             = context;
    api.free_context            = freeContext;
    api.get_status              = status;
    api.free_status             = freeStatus;
    api.select_schema           = select;
    api.is_maintenance_mode     = []() -> int { return nativeBusy; };
    api.start_maintenance       = start;
    api.sync_user_data          = sync;
    api.join_maintenance_thread = join;

    mod::rime::Backend backend;
    QObject::connect(&backend, &mod::rime::Backend::currentSchemaChanged, [&] {
        check(statusBorrows == 0, "schema memory must be released before callbacks");
    });
    backend.refreshCurrentSchema();
    check(backend.currentSchemaId().size() == 400, "schema IDs must not be truncated or read past fixed buffers");
    mod::rime::RimeWrapper first, second;
    first.processKey("a");
    second.processKey("b");
    check(creates == 3, "management and two editors should have three independent sessions");

    int  syncFinished = 0, deployFinished = 0;
    bool syncResult = false, deployResult = true;
    QObject::connect(&backend, &mod::rime::Backend::syncFinished, [&](bool ok) {
        ++syncFinished;
        syncResult = ok;
    });
    QObject::connect(&backend, &mod::rime::Backend::deployFinished, [&](bool ok) {
        ++deployFinished;
        deployResult = ok;
    });
    prepare(true);
    backend.syncUserData();
    check(backend.syncing(), "syncing must be true before the worker starts");
    check(waitUntil([] { return joinEntered.load(); }), "sync must join native maintenance off the UI thread");
    const int calls = sessionCalls;
    backend.redeploy();
    backend.syncUserData();
    check(!backend.selectSchema("blocked"), "schema changes must be gated while syncing");
    backend.refreshCurrentSchema();
    backend.schemaListJson();
    first.clear();
    first.selectCandidate(0);
    check(!second.processKey("c"), "editor input must be gated while sync joins");
    {
        mod::rime::RimeWrapper unused;
        unused.clear();
    }
    check(sessionCalls == calls, "no UI session API may run concurrently with maintenance");
    check(
        backend.syncing() && syncFinished == 0 && deployFinished == 0,
        "completion and flags must wait for the actual join"
    );
    release();
    check(waitUntil([&] { return syncFinished == 1; }), "sync should finish after the join");
    check(!backend.syncing() && syncResult, "real success notification should be reported");
    check(first.processKey("d") && second.processKey("e"), "existing cached editors must recover after sync");
    check(
        first.preeditText() == "d" && second.preeditText() == "e",
        "recovered sessions must remain isolated after pointer ID reuse"
    );

    const int previousCreates = creates;
    prepare(false);
    backend.redeploy();
    check(waitUntil([] { return joinEntered.load(); }), "deploy should enter native join");
    check(backend.deploying() && deployFinished == 0, "deployment must not finish early");
    release();
    check(waitUntil([&] { return deployFinished == 1; }), "deploy should finish after its join");
    check(!deployResult && !backend.deploying(), "native deployment failure must not be reported as success");
    check(lastFullCheck == 1, "manual redeploy must request a full check");
    check(first.processKey("f"), "cached editor must recover after same-schema redeployment");
    check(
        creates == previousCreates + 2,
        "only manager and the editor actually reused should be recreated after deploy"
    );
    check(threadViolations == 0, "maintenance start/join must stay off UI while session calls stay on UI");
    std::cout << (failures ? "FAIL" : "PASS")
              << " Backend: join completion, failure propagation, UI gate, long schema ID, generation recovery\n";
    return failures ? 1 : 0;
}
