#include "RimeWrapper.h"
#include "RimeRuntime.h"

#include <QDir>
#include <QPointer>

namespace mod::rime {
namespace {
void rimeNotificationHandler(void*, RimeSessionId, const char* type, const char* value) {
    RimeRuntime::notification(type, value);
    spdlog::debug("Rime notification: {} {}", type ? type : "", value ? value : "");
}
} // namespace

RimeWrapper::RimeWrapper(QObject* parent) : QObject(parent), Logger("RimeWrapper") {}

RimeWrapper::~RimeWrapper() {
    // Session IDs encode pointers and can be reused after CleanupAllSessions.
    // An old generation must never destroy a new owner's same-valued ID.
    if (validSession()) {
        auto* api = RimeRuntime::api();
        if (api->destroy_session) api->destroy_session(m_sessionId);
    }
}

void RimeWrapper::globalInitialize() {
    static bool initialized = false;
    if (initialized) return;
    auto* api = RimeRuntime::api();
    if (!api || !api->setup || !api->initialize) return;

    RimeTraits traits{};
    RIME_STRUCT_INIT(RimeTraits, traits);
    traits.app_name               = "rime.penmods";
    traits.distribution_name      = "PenMods";
    traits.distribution_code_name = "penmods";
    traits.distribution_version   = "1.0";
    traits.shared_data_dir        = "/userdisk/Music/Rime";
    traits.user_data_dir          = "/userdisk/Music/Rime";
    traits.log_dir                = "/tmp/rime";
    traits.staging_dir            = "/userdisk/Music/Rime/build";
    traits.min_log_level          = 1;
    QDir().mkpath(traits.user_data_dir);
    QDir().mkpath(traits.log_dir);
    QDir().mkpath(traits.staging_dir);

    api->setup(&traits);
    api->initialize(&traits);
    if (api->set_notification_handler) api->set_notification_handler(rimeNotificationHandler, nullptr);
    initialized = true;
    spdlog::info("Rime Global Initialized Successfully");
    // Backend runs startup maintenance through the same gated worker as manual
    // deployment and sync; no session API can race with that worker.
}

QString     RimeWrapper::preeditText() const { return m_preeditText; }
QStringList RimeWrapper::candidates() const { return m_candidates; }
QString     RimeWrapper::schemaId() const { return m_schemaId; }

void RimeWrapper::setSchemaId(const QString& schemaId) {
    if (m_schemaId == schemaId) return;
    m_schemaId = schemaId;
    QPointer<RimeWrapper> alive(this);
    clear();
    if (alive) emit schemaIdChanged();
}

bool RimeWrapper::validSession() {
    const auto generation = RimeRuntime::generation();
    if (m_generation != generation) {
        m_sessionId = 0;
        m_appliedSchema.clear();
        m_generation = generation;
    }
    if (!m_sessionId || !RimeRuntime::available()) return false;
    auto* api = RimeRuntime::api();
    if (api->find_session && !api->find_session(m_sessionId)) {
        m_sessionId = 0;
        m_appliedSchema.clear();
        return false;
    }
    return true;
}

bool RimeWrapper::ensureSession() {
    if (!RimeRuntime::available()) return false;
    auto* api = RimeRuntime::api();
    if (!validSession()) {
        if (!api->create_session) return false;
        m_sessionId = api->create_session();
        if (!m_sessionId) return false;
        m_generation = RimeRuntime::generation();
        debug("Rime session created: {}", m_sessionId);
    }
    if (!m_schemaId.isEmpty() && m_appliedSchema != m_schemaId) {
        const auto utf8 = m_schemaId.toUtf8();
        if (!api->select_schema || !api->select_schema(m_sessionId, utf8.constData())) return false;
        m_appliedSchema = m_schemaId;
    }
    return true;
}

bool RimeWrapper::processKey(const QString& key) {
    int keycode = 0;
    if (key == " " || key == "space") keycode = 0x20; // XK_space, not XK_Multi_key (0xff20).
    else if (key == "\b" || key == "BackSpace" || key == "backspace") keycode = 0xff08;
    else if (key == "\n" || key == "\r" || key == "Return" || key == "Enter") keycode = 0xff0d;
    else if (key == "\t" || key == "Tab") keycode = 0xff09;
    else if (key == "Escape") keycode = 0xff1b;
    else if (key == "PageUp") keycode = 0xff55;
    else if (key == "PageDown") keycode = 0xff56;
    else if (key == "Up") keycode = 0xff52;
    else if (key == "Down") keycode = 0xff54;
    else if (key == "Left") keycode = 0xff51;
    else if (key == "Right") keycode = 0xff53;
    else if (key.size() == 1 && key[0].unicode() >= 0x21 && key[0].unicode() <= 0x7e) keycode = key[0].unicode();
    else return false;

    if (!ensureSession()) return false;
    auto* api = RimeRuntime::api();
    if (!api->process_key) return false;
    const bool handled = api->process_key(m_sessionId, keycode, 0) != 0;
    updateContext();
    return handled;
}

void RimeWrapper::selectCandidate(int index) {
    if (index < 0 || index >= m_candidates.size() || !validSession()) return;
    auto* api      = RimeRuntime::api();
    bool  selected = false;
    if (RIME_API_AVAILABLE(api, select_candidate_on_current_page))
        selected = api->select_candidate_on_current_page(m_sessionId, static_cast<size_t>(index)) != 0;
    else if (api->select_candidate)
        selected = api->select_candidate(m_sessionId, m_candidateOffset + static_cast<size_t>(index)) != 0;
    if (selected) updateContext();
}

void RimeWrapper::clear() {
    if (validSession()) {
        auto* api = RimeRuntime::api();
        if (api->clear_composition) api->clear_composition(m_sessionId);
        // Reset must discard any old unread commit, rather than insert it into
        // the next editor contents through a synchronous QML callback.
        updateContext(false);
    } else {
        m_candidateOffset = 0;
        publishState({}, {});
    }
}

void RimeWrapper::updateContext(bool deliverCommit) {
    auto*       api = RimeRuntime::api();
    QString     preedit;
    QStringList candidates;
    QString     committed;
    m_candidateOffset = 0;

    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    if (api->get_context && api->free_context && api->get_context(m_sessionId, &context)) {
        if (context.composition.preedit) preedit = QString::fromUtf8(context.composition.preedit);
        if (context.menu.page_no >= 0 && context.menu.page_size > 0)
            m_candidateOffset = static_cast<size_t>(context.menu.page_no) * static_cast<size_t>(context.menu.page_size);
        if (context.menu.candidates) {
            for (int i = 0; i < context.menu.num_candidates; ++i)
                candidates.append(
                    QString::fromUtf8(context.menu.candidates[i].text ? context.menu.candidates[i].text : "")
                );
        }
        api->free_context(&context);
    }

    RimeCommit commit{};
    RIME_STRUCT_INIT(RimeCommit, commit);
    if (api->get_commit && api->free_commit && api->get_commit(m_sessionId, &commit)) {
        if (deliverCommit && commit.text) committed = QString::fromUtf8(commit.text);
        api->free_commit(&commit);
    }
    publishState(preedit, candidates, committed);
}

void RimeWrapper::publishState(const QString& preedit, const QStringList& candidates, const QString& commit) {
    const auto revision       = ++m_stateRevision;
    const bool preeditChanged = m_preeditText != preedit;
    const bool menuChanged    = m_candidates != candidates;
    m_preeditText             = preedit;
    m_candidates              = candidates;
    // Release all native output before signals: direct QML handlers may clear
    // the composition, begin maintenance, or destroy this editor.
    QPointer<RimeWrapper> alive(this);
    const auto            generation = RimeRuntime::generation();
    if (preeditChanged) emit preeditTextChanged();
    if (!alive || generation != RimeRuntime::generation() || revision != m_stateRevision) return;
    if (menuChanged) emit candidatesChanged();
    if (!alive || generation != RimeRuntime::generation() || revision != m_stateRevision) return;
    if (!commit.isEmpty()) emit commitText(commit);
}

} // namespace mod::rime
