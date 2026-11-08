// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>

#include "common/service/Singleton.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QVariantMap>

namespace mod {
class HfpBattery;

using BluetoothInterfaces = QMap<QString, QVariantMap>;
using BluetoothObjects    = QMap<QDBusObjectPath, BluetoothInterfaces>;

// Only connected devices with a measured value appear in percentages (address -> 0..100).
// Missing entries mean unknown, never zero. D-Bus is started after QCoreApplication exists.
class BluetoothBattery : public QObject, public Singleton<BluetoothBattery> {
    Q_OBJECT
    Q_PROPERTY(QVariantMap percentages READ percentages NOTIFY percentagesChanged)
    Q_PROPERTY(QVariantMap names READ names NOTIFY namesChanged)

public:
    void               start(const QDBusConnection& bus);
    const QVariantMap& percentages() const { return mPercentages; }
    const QVariantMap& names() const { return mNames; }
    ~BluetoothBattery() override;

signals:
    void percentagesChanged();
    void namesChanged();

private slots:
    void onBluezSignal(const QDBusMessage& message);

private:
    friend Singleton<BluetoothBattery>;
    BluetoothBattery();

    void    reload();
    void    consumeSignal(const QDBusMessage& message);
    void    publish();
    void    clearDevice(const QString& path);
    void    getProperties(const QString& path, const QString& interface);
    void    readGatt(const QString& path);
    void    notifyGatt(const QString& path);
    QString batteryDevice(const QString& characteristic) const;
    bool    connected(const QString& device) const;
    bool    currentRequest(const QString& key, quint64 token, quint64 epoch) const;

    std::unique_ptr<QDBusConnection>   mBus;
    std::unique_ptr<HfpBattery>        mHfp;
    QMap<QString, int>                 mHfpLevels;
    QMap<QString, BluetoothInterfaces> mObjects;
    QMap<QString, quint64>             mRequests;
    QSet<QString>                      mReadAttempted;
    QSet<QString>                      mNotifyAttempted;
    QVariantMap                        mPercentages;
    QVariantMap                        mNames;
    QList<QDBusMessage>                mQueuedSignals;
    quint64                            mEpoch       = 0;
    quint64                            mNextRequest = 0;
    bool                               mLoading     = false;
};

} // namespace mod

Q_DECLARE_METATYPE(mod::BluetoothInterfaces)
Q_DECLARE_METATYPE(mod::BluetoothObjects)
