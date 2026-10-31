#include "rime/RimeRuntime.h"
#include "rime/RimeWrapper.h"
#include <QCoreApplication>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

// Logging configuration is unrelated to the API contract being exercised.
std::atomic<bool> mod::Logger::s_configLoaded{false};
void              mod::Logger::_applyLevelFromConfig(const std::string&) {}

namespace {
RimeApi api{};
struct Fake {
    int                                  creates        = 0;
    int                                  destroys       = 0;
    bool                                 live           = false;
    bool                                 maintenance    = false;
    bool                                 perSessionText = false;
    RimeSessionId                        nextId         = 0;
    std::map<RimeSessionId, std::string> sessions;
    std::map<RimeSessionId, std::string> schemas;
    int                                  schemaSelections   = 0;
    int                                  contextBorrows     = 0;
    int                                  commitBorrows      = 0;
    int                                  absoluteSelections = 0;
    int                                  pageSelections     = 0;
    size_t                               selected           = 0;
    std::vector<int>                     keys;
    std::string                          preedit = "a";
    std::string                          commit;
} fake;
int  failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "  FAIL: " << message << '\n';
    }
}
RimeSessionId createSession() {
    ++fake.creates;
    fake.live         = true;
    const auto id     = fake.nextId ? fake.nextId : static_cast<RimeSessionId>(fake.creates);
    fake.nextId       = 0;
    fake.sessions[id] = "";
    return id;
}
int findSession(RimeSessionId id) { return fake.live && fake.sessions.count(id); }
int destroySession(RimeSessionId id) {
    ++fake.destroys;
    fake.sessions.erase(id);
    return 1;
}
int processKey(RimeSessionId id, int key, int) {
    fake.keys.push_back(key);
    fake.sessions[id] += static_cast<char>(key);
    return 1;
}
int getContext(RimeSessionId id, RimeContext* ctx) {
    ++fake.contextBorrows;
    const auto& text                  = fake.perSessionText ? fake.sessions[id] : fake.preedit;
    ctx->composition.preedit          = const_cast<char*>(text.c_str());
    static RimeCandidate candidates[] = {
        {const_cast<char*>("one"), nullptr, nullptr},
        {const_cast<char*>("two"), nullptr, nullptr}
    };
    ctx->menu.page_no        = 2;
    ctx->menu.page_size      = 5;
    ctx->menu.num_candidates = 2;
    ctx->menu.candidates     = candidates;
    return 1;
}
int freeContext(RimeContext*) {
    --fake.contextBorrows;
    return 1;
}
int getCommit(RimeSessionId, RimeCommit* commit) {
    if (fake.commit.empty()) return 0;
    ++fake.commitBorrows;
    commit->text = strdup(fake.commit.c_str());
    fake.commit.clear();
    return 1;
}
int freeCommit(RimeCommit* commit) {
    --fake.commitBorrows;
    free(commit->text);
    return 1;
}
void clearComposition(RimeSessionId id) {
    fake.preedit.clear();
    fake.sessions[id].clear();
}
int selectSchema(RimeSessionId id, const char* schema) {
    ++fake.schemaSelections;
    fake.schemas[id] = schema;
    return 1;
}
int selectAbsolute(RimeSessionId, size_t index) {
    ++fake.absoluteSelections;
    fake.selected = index;
    return 1;
}
int selectOnPage(RimeSessionId, size_t index) {
    ++fake.pageSelections;
    fake.selected = index;
    return 1;
}
void test(const char* name, const std::function<void()>& body) {
    fake             = {};
    const int before = failures;
    body();
    std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
}
} // namespace
extern "C" RimeApi* rime_get_api() { return &api; }
int                 main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    RIME_STRUCT_INIT(RimeApi, api);
    api.create_session                   = createSession;
    api.find_session                     = findSession;
    api.destroy_session                  = destroySession;
    api.process_key                      = processKey;
    api.get_context                      = getContext;
    api.free_context                     = freeContext;
    api.get_commit                       = getCommit;
    api.free_commit                      = freeCommit;
    api.clear_composition                = clearComposition;
    api.select_schema                    = selectSchema;
    api.select_candidate                 = selectAbsolute;
    api.select_candidate_on_current_page = selectOnPage;
    api.is_maintenance_mode              = []() -> int { return fake.maintenance; };
    test("unused editors and clear do not create sessions", [] {
        mod::rime::RimeWrapper first, second;
        first.clear();
        second.clear();
        check(fake.creates == 0, "constructing or resetting an unused editor must not create a session");
    });
    test("one editor reuses its session", [] {
        mod::rime::RimeWrapper wrapper;
        wrapper.processKey("a");
        wrapper.clear();
        wrapper.processKey("b");
        check(fake.creates == 1, "input/reset/input must reuse one session");
    });
    test("X11 space and control keysyms", [] {
        mod::rime::RimeWrapper wrapper;
        wrapper.processKey(" ");
        wrapper.processKey("space");
        wrapper.processKey("\n");
        wrapper.processKey("\t");
        wrapper.processKey("Tab");
        wrapper.processKey("Return");
        check(
            fake.keys == std::vector<int>({0x20, 0x20, 0xff0d, 0xff09, 0xff09, 0xff0d}),
            "space is 0x20; newline and tab are their control keysyms"
        );
    });
    test("candidate indices refer to the displayed page", [] {
        mod::rime::RimeWrapper wrapper;
        wrapper.processKey("a");
        wrapper.selectCandidate(1);
        check(
            fake.pageSelections == 1 && fake.selected == 1,
            "displayed page index must use select_candidate_on_current_page"
        );
    });
    test("invalid candidate indices never reach librime", [] {
        mod::rime::RimeWrapper wrapper;
        wrapper.processKey("a");
        wrapper.selectCandidate(-1);
        wrapper.selectCandidate(2);
        check(
            fake.absoluteSelections + fake.pageSelections == 0,
            "negative/out-of-page candidate indices must be rejected"
        );
    });
    test("context and commit are freed before direct Qt callbacks", [] {
        mod::rime::RimeWrapper wrapper;
        QObject::connect(&wrapper, &mod::rime::RimeWrapper::preeditTextChanged, [&] {
            check(fake.contextBorrows == 0, "preedit signal must not expose a borrowed RimeContext");
        });
        QObject::connect(&wrapper, &mod::rime::RimeWrapper::candidatesChanged, [&] {
            check(fake.contextBorrows == 0, "candidate signal must not expose a borrowed RimeContext");
        });
        QObject::connect(&wrapper, &mod::rime::RimeWrapper::commitText, [&](const QString& text) {
            check(text == "committed", "commit text must remain valid after releasing API memory");
            check(fake.commitBorrows == 0, "commit must be freed before a reentrant QML callback");
            wrapper.clear();
        });
        fake.commit = "committed";
        wrapper.processKey("a");
        check(fake.commitBorrows == 0 && fake.contextBorrows == 0, "each API output must be freed exactly once");
    });
    test("sessions removed by sync recover on the next key", [] {
        mod::rime::RimeWrapper wrapper;
        wrapper.processKey("a");
        fake.live = false;
        wrapper.processKey("b");
        check(fake.creates == 2, "a nonzero session id must be validated and recreated after CleanupAllSessions");
    });
    test("maintenance rejects input without creating sessions", [] {
        mod::rime::RimeWrapper wrapper;
        fake.maintenance = true;
        check(!wrapper.processKey("a"), "maintenance must not accept input");
        check(fake.creates == 0 && fake.keys.empty(), "maintenance must not allocate or access a session");
    });
    test("unsupported Unicode is never truncated to a Latin-1 key", [] {
        mod::rime::RimeWrapper wrapper;
        check(!wrapper.processKey(QString::fromUtf8("汉")), "unsupported text should be returned to the caller");
        check(fake.keys.empty(), "Unicode must not become a NUL keysym");
    });
    test("editors keep independent compositions and lifetimes", [] {
        fake.perSessionText          = true;
        auto                   first = std::make_unique<mod::rime::RimeWrapper>();
        mod::rime::RimeWrapper second;
        first->processKey("a");
        second.processKey("b");
        check(first->preeditText() == "a" && second.preeditText() == "b", "editors must not share composition");
        first->clear();
        first.reset();
        second.processKey("c");
        check(second.preeditText() == "bc" && fake.creates == 2, "clearing and destroying A must preserve B");
    });
    test("retired wrappers cannot destroy a reused session ID", [] {
        auto retired = std::make_unique<mod::rime::RimeWrapper>();
        retired->processKey("a");
        check(mod::rime::RimeRuntime::beginMaintenance(), "maintenance should reserve the API boundary");
        fake.sessions.clear();
        mod::rime::RimeRuntime::endMaintenance();
        fake.nextId = 1;
        mod::rime::RimeWrapper current;
        current.processKey("b");
        retired->clear();
        retired.reset();
        check(
            fake.sessions.count(1) && fake.destroys == 0,
            "old epoch must not clear or destroy a new owner with the same ID"
        );
        current.processKey("c");
        check(fake.creates == 2, "new owner should continue reusing its session");
    });
    test("pending maintenance gates all editor session operations", [] {
        auto wrapper = std::make_unique<mod::rime::RimeWrapper>();
        wrapper->processKey("a");
        check(mod::rime::RimeRuntime::beginMaintenance(), "maintenance reservation should succeed");
        wrapper->clear();
        wrapper->selectCandidate(0);
        check(!wrapper->processKey("b"), "input must be rejected before the worker even enters native maintenance");
        wrapper.reset();
        check(
            fake.keys.size() == 1 && fake.creates == 1 && fake.destroys == 0,
            "no session API during pending maintenance"
        );
        fake.sessions.clear();
        mod::rime::RimeRuntime::notification("deploy", "failure");
        check(!mod::rime::RimeRuntime::maintenanceSucceeded(), "failure notification must not be reported as success");
        mod::rime::RimeRuntime::endMaintenance();
    });
    test("schema changes reuse the editor and select once per change", [] {
        mod::rime::RimeWrapper wrapper;
        wrapper.setSchemaId("alpha");
        wrapper.clear();
        check(fake.creates == 0, "binding a schema on an unused keyboard must be lazy");
        wrapper.processKey("a");
        wrapper.processKey("b");
        check(fake.schemaSelections == 1, "repeated keys must not select the schema repeatedly");
        wrapper.setSchemaId("beta");
        wrapper.processKey("c");
        check(
            fake.creates == 1 && fake.schemaSelections == 2 && fake.schemas[1] == "beta",
            "switch scheme without rebuilding keyboard or session"
        );
    });
    test("old APIs use the displayed page offset", [] {
        api.select_candidate_on_current_page = nullptr;
        mod::rime::RimeWrapper wrapper;
        wrapper.processKey("a");
        wrapper.selectCandidate(1);
        check(
            fake.absoluteSelections == 1 && fake.selected == 11,
            "fallback needs page_no times page_size plus displayed index"
        );
        api.select_candidate_on_current_page = selectOnPage;
    });
    test("preedit callbacks can cancel an unread commit", [] {
        mod::rime::RimeWrapper wrapper;
        int                    commits = 0;
        QObject::connect(&wrapper, &mod::rime::RimeWrapper::commitText, [&](const QString&) { ++commits; });
        QObject::connect(&wrapper, &mod::rime::RimeWrapper::preeditTextChanged, [&] { wrapper.clear(); });
        fake.commit = "cancelled";
        wrapper.processKey("a");
        check(
            commits == 0 && wrapper.preeditText().isEmpty(),
            "clear during preedit notification must cancel the outer commit"
        );
    });
    test("candidate callbacks can cancel an unread commit", [] {
        mod::rime::RimeWrapper wrapper;
        int                    commits = 0;
        QObject::connect(&wrapper, &mod::rime::RimeWrapper::commitText, [&](const QString&) { ++commits; });
        QObject::connect(&wrapper, &mod::rime::RimeWrapper::candidatesChanged, [&] { wrapper.clear(); });
        fake.commit = "cancelled";
        wrapper.processKey("a");
        check(commits == 0, "clear during candidate notification must cancel the outer commit");
    });
    test("schema reset permits synchronous deletion", [] {
        auto* wrapper = new mod::rime::RimeWrapper;
        wrapper->processKey("a");
        QObject::connect(wrapper, &mod::rime::RimeWrapper::preeditTextChanged, [&] {
            delete wrapper;
            wrapper = nullptr;
        });
        wrapper->setSchemaId("beta");
        check(wrapper == nullptr, "schema setter must stop after a reset callback destroys the editor");
    });
    std::cout << failures << " assertion failure(s)\n";
    return failures ? 1 : 0;
}
