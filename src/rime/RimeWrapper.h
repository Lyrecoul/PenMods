#ifndef RIMEWRAPPER_H
#define RIMEWRAPPER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>

#include "RimeApiWrapper.h"
#include "common/service/Logger.h"

namespace mod::rime {

// Each editor keeps its own composition. Native sessions are allocated only
// when the editor first sends a pinyin key, and recovered after maintenance.
class RimeWrapper : public QObject, private Logger {
    Q_OBJECT
    Q_PROPERTY(QString preeditText READ preeditText NOTIFY preeditTextChanged)
    Q_PROPERTY(QStringList candidates READ candidates NOTIFY candidatesChanged)
    Q_PROPERTY(QString schemaId READ schemaId WRITE setSchemaId NOTIFY schemaIdChanged)

public:
    explicit RimeWrapper(QObject* parent = nullptr);
    ~RimeWrapper();

    QString     preeditText() const;
    QStringList candidates() const;
    QString     schemaId() const;
    void        setSchemaId(const QString& schemaId);

    Q_INVOKABLE bool processKey(const QString& key);
    Q_INVOKABLE void selectCandidate(int index);
    Q_INVOKABLE void clear();

    static void globalInitialize();

signals:
    void preeditTextChanged();
    void candidatesChanged();
    void schemaIdChanged();
    void commitText(const QString& text);

private:
    bool validSession();
    bool ensureSession();
    void updateContext(bool deliverCommit = true);
    void publishState(const QString& preedit, const QStringList& candidates, const QString& commit = {});

    RimeSessionId m_sessionId     = 0;
    uint64_t      m_generation    = 0;
    uint64_t      m_stateRevision = 0;
    QString       m_schemaId;
    QString       m_appliedSchema;
    QString       m_preeditText;
    QStringList   m_candidates;
    size_t        m_candidateOffset = 0;
};

} // namespace mod::rime

#endif // RIMEWRAPPER_H
