#ifndef RIME_BACKEND_H
#define RIME_BACKEND_H

#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

#include "RimeWrapper.h"

namespace mod::rime {

/**
 * Rime 运行时管理后端（部署 / 输入方案切换 / 用户数据同步）。
 *
 * 与 RimeWrapper 的区别：RimeWrapper 是每次打开输入页都会重新实例化的会话对象，
 * 而 Backend 是全局单例，持有一个常驻会话用于查询与修改 Rime 的全局状态。
 */
class Backend : public QObject, public Singleton<Backend>, private Logger {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool deploying READ deploying NOTIFY deployingChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString currentSchemaId READ currentSchemaId NOTIFY currentSchemaChanged)

public:
    explicit Backend();

    void initialize(QQmlEngine* engine, QJSEngine* scriptEngine);

    bool deploying() const { return m_deploying; }
    bool syncing() const { return m_syncing; }

    QString currentSchemaId() const { return m_currentSchemaId; }

    /// 返回已部署的输入方案列表，格式为 JSON 数组: [{"id": "...", "name": "..."}]
    Q_INVOKABLE QString schemaListJson();

    /// 切换当前输入方案，成功时返回 true。
    Q_INVOKABLE bool selectSchema(const QString& schemaId);

    /// 异步重新部署（编译/加载词库与方案），完成后发出 deployFinished。
    Q_INVOKABLE void redeploy();

    /// 异步同步用户数据（用户词典等），完成后发出 syncFinished。
    Q_INVOKABLE void syncUserData();

    /// 重新读取当前输入方案并发出 currentSchemaChanged。
    Q_INVOKABLE void refreshCurrentSchema();

signals:
    void deployingChanged();
    void syncingChanged();
    void currentSchemaChanged();
    void deployFinished(bool success);
    void syncFinished(bool success);

private:
    RimeSessionId ensureSession();

    RimeWrapper*  m_rimeWrapper;
    RimeSessionId m_sessionId = 0;
    QString       m_currentSchemaId;
    bool          m_deploying = false;
    bool          m_syncing   = false;
    QThreadPool   m_workerPool;
};

} // namespace mod::rime

#endif // RIME_BACKEND_H
