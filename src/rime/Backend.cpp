#include "Backend.h"
#include "RimeRuntime.h"
#include "RimeWrapper.h"
#include "common/Event.h"

#include <QRunnable>
#include <nlohmann/json.hpp>

namespace mod::rime {
namespace {
template <typename Function>
class RunnableTask final : public QRunnable {
public:
    explicit RunnableTask(Function function) : m_function(std::move(function)) {}
    void run() override { m_function(); }

private:
    Function m_function;
};
} // namespace

Backend::Backend() : Logger("Rime") {
    m_workerPool.setMaxThreadCount(1);
    m_workerPool.setExpiryTimeout(30000);
    connect(&Event::getInstance(), &Event::beforeUiInitialization, this, [this](QQuickView&, QQmlContext* context) {
        initialize(context->engine(), nullptr);
        context->setContextProperty("rime", this);
    });
}

Backend::~Backend() {
    m_workerPool.waitForDone();
    if (m_deploying || m_syncing) RimeRuntime::endMaintenance();
    // The process-wide management session is owned until native service cleanup.
    // Do not call librime here: its static service may already be destroyed.
}

void Backend::initialize(QQmlEngine* engine, QJSEngine* scriptEngine) {
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    qmlRegisterType<RimeWrapper>("com.youdao.input", 1, 0, "RimeWrapper");
    RimeWrapper::globalInitialize();
    startMaintenance(Maintenance::Startup);
}

RimeSessionId Backend::ensureSession() {
    if (!RimeRuntime::available()) return 0;
    auto* api = RimeRuntime::api();
    if (!api->create_session) return 0;
    if (m_generation != RimeRuntime::generation()) {
        m_sessionId  = 0;
        m_generation = RimeRuntime::generation();
    }
    if (m_sessionId && api->find_session && !api->find_session(m_sessionId)) m_sessionId = 0;
    if (!m_sessionId) {
        m_sessionId = api->create_session();
        if (m_sessionId) info("Rime management session created: {}", m_sessionId);
    }
    return m_sessionId;
}

QString Backend::schemaListJson() {
    nlohmann::json result = nlohmann::json::array();
    if (!RimeRuntime::available()) return QString::fromStdString(result.dump());
    auto* api = RimeRuntime::api();
    if (!api->get_schema_list || !api->free_schema_list) return QString::fromStdString(result.dump());
    RimeSchemaList list{};
    if (api->get_schema_list(&list)) {
        for (size_t i = 0; i < list.size; ++i) {
            const auto& item = list.list[i];
            result.push_back({
                {"id",   item.schema_id ? item.schema_id : ""},
                {"name", item.name ? item.name : ""          }
            });
        }
        api->free_schema_list(&list);
    }
    return QString::fromStdString(result.dump());
}

bool Backend::selectSchema(const QString& schemaId) {
    if (schemaId.isEmpty() || !RimeRuntime::available()) return false;
    auto* api = RimeRuntime::api();
    if (!api->select_schema) return false;
    const auto session = ensureSession();
    if (!session) return false;
    const auto utf8 = schemaId.toUtf8();
    if (!api->select_schema(session, utf8.constData())) return false;
    info("Rime schema switched to {}", utf8.constData());
    refreshCurrentSchema();
    return true;
}

void Backend::refreshCurrentSchema() {
    if (!RimeRuntime::available()) return;
    auto* api = RimeRuntime::api();
    if (!api->get_status || !api->free_status) return;
    const auto session = ensureSession();
    if (!session) return;
    RimeStatus status{};
    RIME_STRUCT_INIT(RimeStatus, status);
    if (!api->get_status(session, &status)) return;
    const auto id = QString::fromUtf8(status.schema_id ? status.schema_id : "");
    api->free_status(&status);
    if (m_currentSchemaId != id) {
        m_currentSchemaId = id;
        emit currentSchemaChanged();
    }
}

void Backend::redeploy() { startMaintenance(Maintenance::Deploy); }
void Backend::syncUserData() { startMaintenance(Maintenance::Sync); }

void Backend::startMaintenance(Maintenance operation) {
    if (m_deploying || m_syncing) return;
    auto*      api  = RimeRuntime::api();
    const bool sync = operation == Maintenance::Sync;
    if (!api || !api->join_maintenance_thread || !api->cleanup_all_sessions
        || (sync ? !api->sync_user_data : !api->start_maintenance)) {
        if (sync) emit syncFinished(false);
        else emit deployFinished(false);
        return;
    }
    // Reserve and invalidate on the GUI thread before a worker can clear the
    // session map. All editor/manager calls and destructors honor this gate.
    if (!RimeRuntime::beginMaintenance()) return;
    m_sessionId  = 0;
    m_generation = RimeRuntime::generation();
    if (sync) {
        m_syncing = true;
        emit syncingChanged();
    } else {
        m_deploying = true;
        emit deployingChanged();
    }

    m_workerPool.start(new RunnableTask([this, api, operation, sync] {
        // start_maintenance also performs synchronous filesystem work. Keep
        // its entire invocation and join off the UI thread, with APIs gated.
        // sync_user_data clears sessions itself; ordinary deploy does not.
        if (!sync) api->cleanup_all_sessions();
        const bool started =
            sync ? api->sync_user_data() != 0 : api->start_maintenance(operation == Maintenance::Deploy) != 0;
        if (started) api->join_maintenance_thread();
        const bool ok = started ? RimeRuntime::maintenanceSucceeded()
                                : operation == Maintenance::Startup; // No changed files is normal at startup.
        QMetaObject::invokeMethod(
            this,
            [this, sync, ok] {
                RimeRuntime::endMaintenance();
                if (sync) m_syncing = false;
                else m_deploying = false;
                refreshCurrentSchema();
                if (sync) {
                    emit syncingChanged();
                    emit syncFinished(ok);
                } else {
                    emit deployingChanged();
                    emit deployFinished(ok);
                }
            },
            Qt::QueuedConnection
        );
    }));
}

} // namespace mod::rime
