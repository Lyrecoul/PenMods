// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <functional>

class QProcess;

namespace mod::chatbot {

// Shared by both rendering callers. The installed generation is the only server source.
class MathServerRuntime final : public QObject {
public:
    using Save = std::function<bool(const QJsonObject&)>;
    MathServerRuntime(QString managedPath, Save save, QObject* parent = nullptr, quint16 port = 3000);
    ~MathServerRuntime() override;

    void        load(const QJsonObject& config);
    bool        update(const QJsonObject& patch);
    QJsonObject config() const { return mConfig; }
    bool        installed() const;
    bool        enabled() const;
    QString     path() const;
    QString     status() const { return mStatus; }
    bool        running() const;
    bool        setEnabled(bool enabled);
    bool        start(); // accepted / already owned; HTTP health is checked by callers
    void        stop();
    qint64      processId() const;

    std::function<void()>               changed;
    std::function<void()>               selectionChanged;
    std::function<void(const QString&)> failed;

private:
    struct Identity {
        qint64     pid = 0;
        QByteArray startTime;
        QString    executable;
    };
    QString            mManagedPath, mStoreRoot, mStatus, mRunningPath;
    QStringList        mRetiredPaths;
    Save               mSave;
    QJsonObject        mConfig;
    QPointer<QProcess> mProcess;
    QTimer             mTransition;
    QElapsedTimer      mStoppingSince;
    QList<Identity>    mRetiring;
    quint16            mPort;
    bool               mWanted = false, mStopping = false, mRetirementStarted = false;

    bool            commit(const QJsonObject& config);
    void            drive();
    void            state(const QString& text);
    void            error(const QString& text);
    QList<Identity> oldServers() const;
    static Identity identity(qint64 pid);
    static bool     sameProcess(const Identity& identity);
    static bool     recognized(const QString& path);
};

} // namespace mod::chatbot
