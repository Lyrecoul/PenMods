// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QVariantMap>
#include <memory>

namespace mod {
class HfpBatteryConnection;

// HFP control only: never opens SCO or claims a telephony/audio backend.
class HfpBattery : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Profile1")
public:
    explicit HfpBattery(const QDBusConnection& bus, QObject* parent = nullptr);
    ~HfpBattery() override;
    void reset();
    void sync(const QSet<QString>& devices);

public slots:
    void Release();
    void NewConnection(const QDBusObjectPath& device, const QDBusUnixFileDescriptor& fd, const QVariantMap& properties);
    void RequestDisconnection(const QDBusObjectPath& device);

signals:
    void batteryChanged(const QString& device, int percentage);

private:
    void                                 connectDevice(const QString& device);
    void                                 closeDevice(const QString& device);
    bool                                 fromBluez() const;
    QDBusConnection                      mBus;
    QString                              mOwner;
    QMap<QString, HfpBatteryConnection*> mConnections;
    QSet<QString>                        mDevices, mAttempted;
    quint64                              mEpoch      = 0;
    bool                                 mRegistered = false;
    bool                                 mExported   = false;
};
} // namespace mod
