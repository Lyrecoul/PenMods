#include "Backend.h"
#include "RimeWrapper.h"
#include "common/Event.h"

#include "rime_api.h"

#include <nlohmann/json.hpp>

#include <QRunnable>

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

Backend::Backend() : Logger("Rime"), m_rimeWrapper(nullptr) {
    m_workerPool.setMaxThreadCount(1);
    m_workerPool.setExpiryTimeout(30000);

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        Q_UNUSED(view);
        initialize(context->engine(), nullptr);
        context->setContextProperty("rime", this);
    });
}

void Backend::initialize(QQmlEngine* engine, QJSEngine* scriptEngine) {
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);

    // 1. 注册 QML 类型
    qmlRegisterType<RimeWrapper>("com.youdao.input", 1, 0, "RimeWrapper");

    // 2. 执行 Rime 的全局初始化 (加载字典等耗时操作)
    // 这样当 QML 创建 RimeWrapper 实例时，Backend 已经准备好了
    RimeWrapper::globalInitialize();

    // 初始化时常驻会话可能还不存在，这里只读取一次当前方案用于 QML 展示
    refreshCurrentSchema();

    // 注意：不需要创建 m_rimeWrapper 实例给 setContextProperty
    // 因为 QML 文件里自己写了 "RimeWrapper { id: ... }"
    // QML 会自己 new 一个出来，那个 new 出来的实例会自动调用构造函数创建 Session

    qInfo() << "Rime backend registered and initialized";
}

RimeSessionId Backend::ensureSession() {
    RimeApi* api = rime_get_api();
    if (!api || !api->create_session) return 0;

    if (m_sessionId && api->find_session && !api->find_session(m_sessionId)) {
        m_sessionId = 0;
    }

    if (!m_sessionId) {
        m_sessionId = api->create_session();
        if (m_sessionId) {
            info("Rime management session created: {}", m_sessionId);
        } else {
            error("Failed to create Rime management session");
        }
    }
    return m_sessionId;
}

QString Backend::schemaListJson() {
    nlohmann::json result = nlohmann::json::array();

    RimeApi* api = rime_get_api();
    if (!api || !api->get_schema_list || !api->free_schema_list) {
        return QString::fromStdString(result.dump());
    }

    RimeSchemaList list = {0};
    if (api->get_schema_list(&list)) {
        for (size_t i = 0; i < list.size; ++i) {
            const auto& item = list.list[i];
            const char* id   = item.schema_id ? item.schema_id : "";
            const char* name = item.name ? item.name : id;
            result.push_back({
                {"id",   id  },
                {"name", name}
            });
        }
        api->free_schema_list(&list);
    }

    return QString::fromStdString(result.dump());
}

bool Backend::selectSchema(const QString& schemaId) {
    RimeApi* api = rime_get_api();
    if (!api || !api->select_schema) return false;

    const RimeSessionId session = ensureSession();
    if (!session) return false;

    const QByteArray utf8 = schemaId.toUtf8();
    const bool       ok   = api->select_schema(session, utf8.constData()) != 0;
    if (ok) {
        info("Rime schema switched to {}", utf8.constData());
        refreshCurrentSchema();
    } else {
        error("Failed to switch Rime schema to {}", utf8.constData());
    }
    return ok;
}

void Backend::refreshCurrentSchema() {
    RimeApi* api = rime_get_api();
    if (!api || !api->get_current_schema) return;

    const RimeSessionId session = ensureSession();
    if (!session) return;

    char buffer[256] = {0};
    if (api->get_current_schema(session, buffer, sizeof(buffer))) {
        const QString id = QString::fromUtf8(buffer);
        if (id != m_currentSchemaId) {
            m_currentSchemaId = id;
            emit currentSchemaChanged();
        }
    }
}

void Backend::redeploy() {
    if (m_deploying) return;

    RimeApi* api = rime_get_api();
    if (!api || !api->start_maintenance) {
        emit deployFinished(false);
        return;
    }

    // start_maintenance 只负责拉起维护线程，在 UI 线程调用即可；
    // join_maintenance_thread 会阻塞等待，放到后台线程，避免卡住界面。
    if (!api->start_maintenance(True)) {
        error("Failed to start Rime maintenance");
        emit deployFinished(false);
        return;
    }

    m_deploying = true;
    emit deployingChanged();

    m_workerPool.start(new RunnableTask([this]() {
        RimeApi* threadApi = rime_get_api();
        if (threadApi && threadApi->join_maintenance_thread) {
            threadApi->join_maintenance_thread();
        }

        QMetaObject::invokeMethod(
            this,
            [this]() {
                m_deploying = false;
                emit deployingChanged();
                refreshCurrentSchema();
                emit deployFinished(true);
            },
            Qt::QueuedConnection
        );
    }));
}

void Backend::syncUserData() {
    if (m_syncing) return;

    RimeApi* api = rime_get_api();
    if (!api || !api->sync_user_data) {
        emit syncFinished(false);
        return;
    }

    m_syncing = true;
    emit syncingChanged();

    m_workerPool.start(new RunnableTask([this]() {
        RimeApi*   threadApi = rime_get_api();
        const bool ok        = threadApi && threadApi->sync_user_data && threadApi->sync_user_data() != 0;

        QMetaObject::invokeMethod(
            this,
            [this, ok]() {
                m_syncing = false;
                emit syncingChanged();
                emit syncFinished(ok);
            },
            Qt::QueuedConnection
        );
    }));
}

} // namespace mod::rime
