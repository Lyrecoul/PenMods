// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "mod/Config.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

namespace mod {

// A separate, stateless translation request: never changes ChatBot's model, prompts or sessions.
class AiTranslation : public QObject, public Singleton<AiTranslation> {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString modelId READ modelId WRITE setModelId NOTIFY settingsChanged)
    Q_PROPERTY(int timeoutMs READ timeoutMs WRITE setTimeoutMs NOTIFY settingsChanged)
    Q_PROPERTY(QString text READ text NOTIFY resultChanged)
    Q_PROPERTY(QString source READ source NOTIFY resultChanged)
    Q_PROPERTY(QString target READ target NOTIFY resultChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY resultChanged)
    Q_PROPERTY(QString status READ status NOTIFY resultChanged)

public:
    bool             enabled() const { return mEnabled; }
    QString          modelId() const { return mModelId; }
    int              timeoutMs() const { return mTimeoutMs; }
    QString          text() const { return mText; }
    QString          source() const { return mSource; }
    QString          target() const { return mTarget; }
    bool             busy() const { return mBusy; }
    QString          status() const { return mStatus; }
    void             setEnabled(bool value);
    void             setModelId(const QString& value);
    void             setTimeoutMs(int value);
    Q_INVOKABLE void translate(const QString& source, const QString& target, bool online);
    Q_INVOKABLE void cancel();

signals:
    void settingsChanged();
    void resultChanged();

private:
    friend Singleton<AiTranslation>;
    AiTranslation();
    void saveSettings();
    void fail(const QString& reason);
    void consume(QNetworkReply* reply);
    void parseEvent(const QByteArray& data);
    void parseObject(const QJsonObject& object, bool streaming);
    void append(const QString& delta);
    void finish(QNetworkReply* reply);

    bool                    mEnabled = false;
    QString                 mModelId;
    int                     mTimeoutMs = 15000;
    QNetworkAccessManager*  mNetwork   = nullptr;
    QPointer<QNetworkReply> mReply;
    QTimer                  mDeadline;
    QString                 mText, mSource, mTarget, mStatus, mProtocol;
    QByteArray              mBuffer, mEventData;
    int                     mReceived = 0;
    bool                    mBusy     = false;
    bool                    mSse      = false;
    bool                    mComplete = false;
    bool                    mInvalid  = false;
};

} // namespace mod
