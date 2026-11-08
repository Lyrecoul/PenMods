// SPDX-License-Identifier: GPL-3.0-only
#include "helper/BluetoothBattery.h"
#include "helper/HfpBattery.h"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace mod {
namespace {
const QString kDevice         = QStringLiteral("org.bluez.Device1");
const QString kBattery        = QStringLiteral("org.bluez.Battery1");
const QString kService        = QStringLiteral("org.bluez.GattService1");
const QString kCharacteristic = QStringLiteral("org.bluez.GattCharacteristic1");
const QString kProperties     = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kManager        = QStringLiteral("org.freedesktop.DBus.ObjectManager");

bool uuidIs(const QVariant& value, const char* shortUuid) {
    return value.toString().compare(
               QString::fromLatin1(shortUuid) + QStringLiteral("-0000-1000-8000-00805f9b34fb"),
               Qt::CaseInsensitive
           )
        == 0;
}

int percentage(const QVariant& value) {
    bool      ok     = false;
    const int result = value.toInt(&ok);
    return ok && result >= 0 && result <= 100 ? result : -1;
}

QDBusMessage call(const QString& path, const QString& interface, const QString& method) {
    return QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), path, interface, method);
}
} // namespace

BluetoothBattery::BluetoothBattery()  = default;
BluetoothBattery::~BluetoothBattery() = default;

void BluetoothBattery::start(const QDBusConnection& bus) {
    if (mBus) return;
    qDBusRegisterMetaType<BluetoothInterfaces>();
    qDBusRegisterMetaType<BluetoothObjects>();
    mBus = std::make_unique<QDBusConnection>(bus);
    mHfp = std::make_unique<HfpBattery>(bus);
    connect(mHfp.get(), &HfpBattery::batteryChanged, this, [this](const QString& path, int level) {
        if (level >= 0 && connected(path)) mHfpLevels.insert(path, level);
        else mHfpLevels.remove(path);
        if (!mLoading) publish();
    });
    mBus->connect(
        QStringLiteral("org.bluez"),
        QStringLiteral("/"),
        kManager,
        QStringLiteral("InterfacesAdded"),
        this,
        SLOT(onBluezSignal(QDBusMessage))
    );
    mBus->connect(
        QStringLiteral("org.bluez"),
        QStringLiteral("/"),
        kManager,
        QStringLiteral("InterfacesRemoved"),
        this,
        SLOT(onBluezSignal(QDBusMessage))
    );
    mBus->connect(
        QStringLiteral("org.bluez"),
        QString(),
        kProperties,
        QStringLiteral("PropertiesChanged"),
        this,
        SLOT(onBluezSignal(QDBusMessage))
    );
    mBus->connect(
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("NameOwnerChanged"),
        this,
        SLOT(onBluezSignal(QDBusMessage))
    );
    reload();
}

void BluetoothBattery::reload() {
    const quint64 epoch = ++mEpoch;
    mLoading            = true;
    mHfpLevels.clear();
    mHfp->reset();
    mObjects.clear();
    mRequests.clear();
    mReadAttempted.clear();
    mNotifyAttempted.clear();
    mQueuedSignals.clear();
    if (!mPercentages.isEmpty()) {
        mPercentages.clear();
        emit percentagesChanged();
    }
    if (!mNames.isEmpty()) {
        mNames.clear();
        emit namesChanged();
    }
    mLoading      = true;
    auto* watcher = new QDBusPendingCallWatcher(
        mBus->asyncCall(call(QStringLiteral("/"), kManager, QStringLiteral("GetManagedObjects"))),
        this
    );
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, epoch](QDBusPendingCallWatcher* pending) {
        QDBusPendingReply<BluetoothObjects> reply = *pending;
        pending->deleteLater();
        if (epoch != mEpoch) return;
        if (!reply.isError()) {
            const auto objects = reply.value();
            for (auto it = objects.cbegin(); it != objects.cend(); ++it) mObjects.insert(it.key().path(), it.value());
        }
        mLoading          = false;
        const auto queued = std::move(mQueuedSignals);
        mQueuedSignals.clear();
        for (const auto& signal : queued) consumeSignal(signal);
        publish();
    });
}

void BluetoothBattery::onBluezSignal(const QDBusMessage& message) {
    const auto args = message.arguments();
    if (message.member() == QStringLiteral("NameOwnerChanged")) {
        if (args.size() == 3 && args.at(0).toString() == QStringLiteral("org.bluez")) reload();
        return;
    }
    if (mLoading) {
        mQueuedSignals.append(message);
        return;
    }
    consumeSignal(message);
    publish();
}

bool BluetoothBattery::connected(const QString& device) const {
    return mObjects.value(device).value(kDevice).value(QStringLiteral("Connected")).toBool();
}

QString BluetoothBattery::batteryDevice(const QString& characteristic) const {
    const auto props = mObjects.value(characteristic).value(kCharacteristic);
    if (!uuidIs(props.value(QStringLiteral("UUID")), "00002a19")) return {};
    const QString service      = qvariant_cast<QDBusObjectPath>(props.value(QStringLiteral("Service"))).path();
    const auto    serviceProps = mObjects.value(service).value(kService);
    if (!uuidIs(serviceProps.value(QStringLiteral("UUID")), "0000180f")) return {};
    const QString device = qvariant_cast<QDBusObjectPath>(serviceProps.value(QStringLiteral("Device"))).path();
    // Follow both explicit parent references, not an address guessed from a characteristic name.
    if (!service.startsWith(device + QLatin1Char('/')) || !characteristic.startsWith(service + QLatin1Char('/'))
        || !mObjects.value(device).contains(kDevice))
        return {};
    return device;
}

void BluetoothBattery::clearDevice(const QString& path) {
    mHfpLevels.remove(path);
    for (auto it = mRequests.begin(); it != mRequests.end();) {
        if (it.key().startsWith(path + QLatin1Char('/')) || it.key().startsWith(path + QLatin1Char('|')))
            it = mRequests.erase(it);
        else ++it;
    }
    for (auto* attempted : {&mReadAttempted, &mNotifyAttempted}) {
        for (auto it = attempted->begin(); it != attempted->end();) {
            if (*it == path || it->startsWith(path + QLatin1Char('/'))) it = attempted->erase(it);
            else ++it;
        }
    }
    auto object = mObjects.find(path);
    if (object != mObjects.end() && object->contains(kBattery))
        (*object)[kBattery].remove(QStringLiteral("Percentage"));
    for (auto it = mObjects.begin(); it != mObjects.end(); ++it) {
        if ((it.key() == path || it.key().startsWith(path + QLatin1Char('/'))) && it->contains(kCharacteristic))
            it.value()[kCharacteristic].remove(QStringLiteral("Value"));
    }
}

void BluetoothBattery::consumeSignal(const QDBusMessage& message) {
    const auto args = message.arguments();
    if (message.member() == QStringLiteral("InterfacesAdded") && args.size() == 2) {
        const QString path       = qvariant_cast<QDBusObjectPath>(args.at(0)).path();
        const auto    interfaces = qdbus_cast<BluetoothInterfaces>(args.at(1));
        for (auto it = interfaces.cbegin(); it != interfaces.cend(); ++it) mObjects[path].insert(it.key(), it.value());
    } else if (message.member() == QStringLiteral("InterfacesRemoved") && args.size() == 2) {
        const QString path       = qvariant_cast<QDBusObjectPath>(args.at(0)).path();
        const auto    interfaces = args.at(1).toStringList();
        clearDevice(path);
        for (const auto& interface : interfaces) mObjects[path].remove(interface);
        if (mObjects.value(path).isEmpty()) mObjects.remove(path);
    } else if (message.member() == QStringLiteral("PropertiesChanged") && args.size() == 3) {
        const QString path      = message.path();
        const QString interface = args.at(0).toString();
        if (!mObjects.value(path).contains(interface)) return;
        const auto changed        = qdbus_cast<QVariantMap>(args.at(1));
        const auto invalidated    = args.at(2).toStringList();
        const bool refreshPending = mRequests.remove(path + QLatin1Char('|') + interface) > 0;
        auto&      props          = mObjects[path][interface];
        for (auto it = changed.cbegin(); it != changed.cend(); ++it) props.insert(it.key(), it.value());
        for (const auto& name : invalidated) props.remove(name);
        if (interface == kDevice
            && (changed.contains(QStringLiteral("Connected")) || invalidated.contains(QStringLiteral("Connected")))) {
            clearDevice(path);
            if (connected(path) && mObjects.value(path).contains(kBattery)) getProperties(path, kBattery);
        }
        if (interface == kCharacteristic
            && (changed.contains(QStringLiteral("Value")) || invalidated.contains(QStringLiteral("Value")))) {
            mRequests.remove(path + QStringLiteral("|read"));
        }
        if (refreshPending || !invalidated.isEmpty()) getProperties(path, interface);
    }
}

bool BluetoothBattery::currentRequest(const QString& key, quint64 token, quint64 epoch) const {
    return epoch == mEpoch && mRequests.value(key) == token;
}

void BluetoothBattery::getProperties(const QString& path, const QString& interface) {
    const QString key   = path + QLatin1Char('|') + interface;
    const quint64 token = ++mNextRequest, epoch = mEpoch;
    mRequests.insert(key, token);
    auto request = call(path, kProperties, QStringLiteral("GetAll"));
    request << interface;
    auto* watcher = new QDBusPendingCallWatcher(mBus->asyncCall(request), this);
    connect(
        watcher,
        &QDBusPendingCallWatcher::finished,
        this,
        [this, path, interface, key, token, epoch](QDBusPendingCallWatcher* pending) {
            QDBusPendingReply<QVariantMap> reply = *pending;
            pending->deleteLater();
            if (!currentRequest(key, token, epoch)) return;
            mRequests.remove(key);
            if (reply.isError() || !mObjects.value(path).contains(interface)) return;
            if (interface == kBattery && !connected(path)) return;
            mObjects[path][interface] = reply.value();
            if (interface == kDevice && !connected(path)) clearDevice(path);
            if (interface == kDevice && connected(path) && mObjects.value(path).contains(kBattery))
                getProperties(path, kBattery);
            publish();
        }
    );
}

void BluetoothBattery::readGatt(const QString& path) {
    mReadAttempted.insert(path);
    const QString key   = path + QStringLiteral("|read");
    const quint64 token = ++mNextRequest, epoch = mEpoch;
    mRequests.insert(key, token);
    auto request = call(path, kCharacteristic, QStringLiteral("ReadValue"));
    request << QVariantMap{};
    auto* watcher = new QDBusPendingCallWatcher(mBus->asyncCall(request), this);
    connect(
        watcher,
        &QDBusPendingCallWatcher::finished,
        this,
        [this, path, key, token, epoch](QDBusPendingCallWatcher* pending) {
            QDBusPendingReply<QByteArray> reply = *pending;
            pending->deleteLater();
            if (!currentRequest(key, token, epoch)) return;
            mRequests.remove(key);
            if (reply.isError() || !connected(batteryDevice(path))) return;
            const auto bytes = reply.value();
            mObjects[path][kCharacteristic].remove(QStringLiteral("Value"));
            if (bytes.size() == 1 && static_cast<unsigned char>(bytes.at(0)) <= 100) {
                mObjects[path][kCharacteristic].insert(QStringLiteral("Value"), bytes);
            }
            publish();
        }
    );
}

void BluetoothBattery::notifyGatt(const QString& path) {
    mNotifyAttempted.insert(path);
    // BlueZ owns notification sessions per D-Bus client; this never stops another client's session.
    auto* watcher =
        new QDBusPendingCallWatcher(mBus->asyncCall(call(path, kCharacteristic, QStringLiteral("StartNotify"))), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, &QObject::deleteLater);
}

void BluetoothBattery::publish() {
    QVariantMap values;
    QVariantMap names;
    for (auto it = mObjects.cbegin(); it != mObjects.cend(); ++it) {
        if (!it.value().contains(kDevice)) continue;
        const auto    props       = it.value().value(kDevice);
        const QString address     = props.value(QStringLiteral("Address")).toString().toUpper();
        const QString remoteName  = props.value(QStringLiteral("Name")).toString();
        QString       displayName = props.value(QStringLiteral("Alias")).toString();
        const QString adapter     = qvariant_cast<QDBusObjectPath>(props.value(QStringLiteral("Adapter"))).path();
        const auto    local       = mObjects.value(adapter).value(QStringLiteral("org.bluez.Adapter1"));
        // Vendor bt_set_device_name can copy the adapter alias onto remote devices.
        // Preserve deliberate remote aliases; do not rewrite pairing data to repair presentation.
        if (displayName.isEmpty()
            || (!remoteName.isEmpty()
                && (displayName == local.value(QStringLiteral("Alias")).toString()
                    || displayName == local.value(QStringLiteral("Name")).toString())))
            displayName = remoteName;
        if (!address.isEmpty() && !displayName.isEmpty()) names.insert(address, displayName);
    }
    if (names != mNames) {
        mNames = names;
        emit namesChanged();
    }
    QStringList   reads, notifications;
    QSet<QString> hfpDevices;
    for (auto it = mObjects.cbegin(); it != mObjects.cend(); ++it) {
        if (!it.value().contains(kDevice) || !connected(it.key())) continue;
        const QString address = it.value().value(kDevice).value(QStringLiteral("Address")).toString().toUpper();
        const int     level   = percentage(it.value().value(kBattery).value(QStringLiteral("Percentage")));
        if (!address.isEmpty() && level >= 0) values.insert(address, level);
        const auto props = it.value().value(kDevice);
        if (props.value(QStringLiteral("Paired")).toBool()
            && props.value(QStringLiteral("UUIDs"))
                   .toStringList()
                   .contains(QStringLiteral("0000111e-0000-1000-8000-00805f9b34fb")))
            hfpDevices.insert(it.key());
        if (!address.isEmpty() && level < 0 && mHfpLevels.contains(it.key()))
            values.insert(address, mHfpLevels.value(it.key()));
    }
    for (auto it = mObjects.cbegin(); it != mObjects.cend(); ++it) {
        if (!it.value().contains(kCharacteristic)) continue;
        const QString device = batteryDevice(it.key());
        if (device.isEmpty() || !connected(device)) continue;
        const auto&   props = it.value().value(kCharacteristic);
        const auto    bytes = props.value(QStringLiteral("Value")).toByteArray();
        const QString address =
            mObjects.value(device).value(kDevice).value(QStringLiteral("Address")).toString().toUpper();
        if (!address.isEmpty() && !values.contains(address) && bytes.size() == 1) {
            const int level = static_cast<unsigned char>(bytes.at(0));
            if (level <= 100) values.insert(address, level);
        }
        const auto flags = props.value(QStringLiteral("Flags")).toStringList();
        if (!mReadAttempted.contains(it.key()) && flags.contains(QStringLiteral("read"))) reads.append(it.key());
        if (!mNotifyAttempted.contains(it.key())
            && (flags.contains(QStringLiteral("notify")) || flags.contains(QStringLiteral("indicate"))))
            notifications.append(it.key());
    }
    if (values != mPercentages) {
        mPercentages = values;
        emit percentagesChanged();
    }
    mHfp->sync(hfpDevices);
    for (const auto& path : reads) readGatt(path);
    for (const auto& path : notifications) notifyGatt(path);
}

} // namespace mod
