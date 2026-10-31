#ifndef RIME_BACKEND_H
#define RIME_BACKEND_H

#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QThreadPool>

#include "RimeApiWrapper.h"
#include "common/service/Logger.h"
#include "common/service/Singleton.h"
#include <cstdint>

namespace mod::rime {

// The management session is separate from each editor's lazy composition.
class Backend : public QObject, public Singleton<Backend>, private Logger {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool deploying READ deploying NOTIFY deployingChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString currentSchemaId READ currentSchemaId NOTIFY currentSchemaChanged)

public:
    explicit Backend();
    ~Backend();
    void    initialize(QQmlEngine* engine, QJSEngine* scriptEngine);
    bool    deploying() const { return m_deploying; }
    bool    syncing() const { return m_syncing; }
    QString currentSchemaId() const { return m_currentSchemaId; }

    Q_INVOKABLE QString schemaListJson();
    Q_INVOKABLE bool    selectSchema(const QString& schemaId);
    Q_INVOKABLE void    redeploy();
    Q_INVOKABLE void    syncUserData();
    Q_INVOKABLE void    refreshCurrentSchema();

signals:
    void deployingChanged();
    void syncingChanged();
    void currentSchemaChanged();
    void deployFinished(bool success);
    void syncFinished(bool success);

private:
    enum class Maintenance { Startup, Deploy, Sync };
    void          startMaintenance(Maintenance operation);
    RimeSessionId ensureSession();

    RimeSessionId m_sessionId  = 0;
    uint64_t      m_generation = 0;
    QString       m_currentSchemaId;
    bool          m_deploying = false;
    bool          m_syncing   = false;
    QThreadPool   m_workerPool;
};

} // namespace mod::rime
#endif // RIME_BACKEND_H
