// SPDX-License-Identifier: GPL-3.0-only
// Real BluetoothBattery against an isolated mock org.bluez. Run tools/test_bluetooth_battery.py.
#include "helper/BluetoothBattery.h"
#include <QCoreApplication>
#include <QDBusMetaType>
#include <QDBusVirtualObject>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <cstdlib>
#include <functional>

using namespace mod;
const QString dev      = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";
const QString svc      = dev + "/service001";
const QString chr      = svc + "/char001";
const QString addr     = "AA:BB:CC:DD:EE:FF";
const QString deviceI  = "org.bluez.Device1";
const QString batteryI = "org.bluez.Battery1";
const QString serviceI = "org.bluez.GattService1";
const QString charI    = "org.bluez.GattCharacteristic1";
const QString propsI   = "org.freedesktop.DBus.Properties";
const QString managerI = "org.freedesktop.DBus.ObjectManager";

class MockBluez : public QDBusVirtualObject {
public:
    BluetoothObjects    objects;
    QByteArray          value = QByteArray(1, char(57));
    int                 reads = 0, notifications = 0;
    bool                holdRead = false, holdSnapshot = false, holdProperties = false;
    QList<QDBusMessage> heldReads, heldSnapshots, heldProperties;
    QDBusConnection     bus;
    MockBluez(QDBusConnection connection) : bus(connection) {}
    QString introspect(const QString&) const override { return {}; }
    bool    handleMessage(const QDBusMessage& msg, const QDBusConnection& connection) override {
        if (msg.interface() == managerI && msg.member() == "GetManagedObjects") {
            if (holdSnapshot) heldSnapshots.append(msg);
            else connection.send(msg.createReply(QVariant::fromValue(objects)));
        } else if (msg.interface() == propsI && msg.member() == "GetAll") {
            if (holdProperties) heldProperties.append(msg);
            else
                connection.send(
                    msg.createReply(objects.value(QDBusObjectPath(msg.path())).value(msg.arguments().at(0).toString()))
                );
        } else if (msg.interface() == charI && msg.member() == "ReadValue") {
            ++reads;
            if (holdRead) heldReads.append(msg);
            else connection.send(msg.createReply(value));
        } else if (msg.interface() == charI && msg.member() == "StartNotify") {
            ++notifications;
            connection.send(msg.createReply());
        } else {
            connection.send(msg.createErrorReply("org.bluez.Error.NotSupported", "fixture unsupported"));
        }
        return true;
    }
    void changed(const QString& path, const QString& interface, QVariantMap changed, QStringList invalidated = {}) {
        auto& props = objects[QDBusObjectPath(path)][interface];
        for (auto it = changed.cbegin(); it != changed.cend(); ++it) props.insert(it.key(), it.value());
        for (const auto& key : invalidated) props.remove(key);
        auto msg = QDBusMessage::createSignal(path, propsI, "PropertiesChanged");
        msg << interface << changed << invalidated;
        bus.send(msg);
    }
    void added(const QString& path, const BluetoothInterfaces& interfaces) {
        objects[QDBusObjectPath(path)] = interfaces;
        auto msg                       = QDBusMessage::createSignal("/", managerI, "InterfacesAdded");
        msg << QVariant::fromValue(QDBusObjectPath(path)) << QVariant::fromValue(interfaces);
        bus.send(msg);
    }
    void removed(const QString& path, const QStringList& interfaces) {
        for (const auto& interface : interfaces) objects[QDBusObjectPath(path)].remove(interface);
        auto msg = QDBusMessage::createSignal("/", managerI, "InterfacesRemoved");
        msg << QVariant::fromValue(QDBusObjectPath(path)) << interfaces;
        bus.send(msg);
    }
};

void spin(int milliseconds = 40) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
void check(bool ok, const char* text) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", text);
        std::exit(1);
    }
    printf("PASS %s\n", text);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qDBusRegisterMetaType<BluetoothInterfaces>();
    qDBusRegisterMetaType<BluetoothObjects>();
    auto serviceBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "mock-bluez");
    auto clientBus  = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "battery-client");
    check(serviceBus.registerService("org.bluez"), "mock service registered");
    MockBluez mock(serviceBus);
    check(serviceBus.registerVirtualObject("/", &mock, QDBusConnection::SubPath), "mock object manager registered");
    mock.objects[QDBusObjectPath(dev)] = {
        {deviceI,  {{"Address", addr}, {"Connected", true}}},
        {batteryI, {{"Percentage", uchar(81)}}             }
    };
    auto& battery = BluetoothBattery::getInstance();
    battery.start(clientBus);
    spin();
    auto level = [&]() { return battery.percentages().value(addr, -1).toInt(); };
    check(level() == 81, "already-connected startup Battery1 81%");
    mock.changed(
        dev,
        batteryI,
        {
            {"Percentage", uchar(0)}
    }
    );
    spin();
    check(level() == 0, "measured zero remains legitimate zero");
    mock.changed(
        dev,
        batteryI,
        {
            {"Percentage", uchar(255)}
    }
    );
    spin();
    check(level() == -1, "out-of-range Battery1 becomes unknown");
    mock.changed(
        dev,
        batteryI,
        {
            {"Percentage", uchar(64)}
    }
    );
    spin();
    mock.changed(
        dev,
        deviceI,
        {
            {"Connected", false}
    }
    );
    spin();
    check(level() == -1, "disconnect clears Battery1");
    mock.changed(
        dev,
        deviceI,
        {
            {"Connected", true}
    }
    );
    spin();
    check(level() == 64, "reconnect asynchronously rereads Battery1");
    mock.holdProperties = true;
    mock.changed(dev, deviceI, {}, {"Connected"});
    spin();
    check(level() == -1 && mock.heldProperties.size() == 1, "Connected invalidation queues GetAll");
    mock.objects[QDBusObjectPath(dev)][deviceI].insert("Connected", true);
    const auto oldDeviceReply = mock.objects.value(QDBusObjectPath(dev)).value(deviceI);
    mock.changed(
        dev,
        deviceI,
        {
            {"Alias", "Updated while read pending"}
    }
    );
    spin();
    check(mock.heldProperties.size() == 2, "unrelated Alias update preserves pending Connected refresh");
    serviceBus.send(mock.heldProperties.takeFirst().createReply(oldDeviceReply));
    spin();
    check(level() == -1, "superseded GetAll does not overwrite newer deltas");
    mock.holdProperties = false;
    serviceBus.send(
        mock.heldProperties.takeFirst().createReply(mock.objects.value(QDBusObjectPath(dev)).value(deviceI))
    );
    spin();
    check(level() == 64, "replacement GetAll restores connection and battery");
    mock.holdProperties = true;
    mock.changed(dev, batteryI, {}, {"Percentage"});
    spin();
    mock.objects[QDBusObjectPath(dev)][batteryI].insert("Percentage", uchar(64));
    mock.changed(
        dev,
        batteryI,
        {
            {"Source", "fixture"}
    }
    );
    spin();
    check(mock.heldProperties.size() == 2, "unrelated Battery1 property preserves Percentage refresh");
    mock.holdProperties = false;
    for (const auto& request : mock.heldProperties)
        serviceBus.send(request.createReply(mock.objects.value(QDBusObjectPath(dev)).value(batteryI)));
    mock.heldProperties.clear();
    spin();
    check(level() == 64, "replacement Battery1 GetAll supplies missing percentage");
    mock.changed(dev, batteryI, {}, {"Percentage"});
    spin();
    check(level() == -1, "invalidated and absent Percentage unknown");
    mock.removed(dev, {batteryI});
    spin();
    check(level() == -1, "missing Battery1 unknown");
    mock.added(
        svc,
        {
            {serviceI,
             {{"UUID", "0000180f-0000-1000-8000-00805f9b34fb"}, {"Device", QVariant::fromValue(QDBusObjectPath(dev))}}}
    }
    );
    mock.added(
        chr,
        {
            {charI,
             {{"UUID", "00002a19-0000-1000-8000-00805f9b34fb"},
              {"Service", QVariant::fromValue(QDBusObjectPath(svc))},
              {"Flags", QStringList{"read", "notify"}}}}
    }
    );
    spin();
    check(
        level() == 57 && mock.reads == 1 && mock.notifications == 1,
        "BAS fallback real async ReadValue and StartNotify"
    );
    mock.changed(
        chr,
        charI,
        {
            {"Value", QByteArray(1, char(42))}
    }
    );
    spin();
    check(level() == 42, "GATT notification updates percentage");
    mock.changed(
        chr,
        charI,
        {
            {"Value", QByteArray(1, char(150))}
    }
    );
    spin();
    check(level() == -1, "invalid GATT percentage clears old value");
    mock.changed(
        chr,
        charI,
        {
            {"Value", QByteArray("ab")}
    }
    );
    spin();
    check(level() == -1, "invalid GATT length unknown");
    mock.changed(
        chr,
        charI,
        {
            {"Value", QByteArray(1, char(0))}
    }
    );
    spin();
    check(level() == 0, "GATT measured zero retained");
    mock.changed(
        svc,
        serviceI,
        {
            {"Device", QVariant::fromValue(QDBusObjectPath("/org/bluez/hci0/dev_OTHER"))}
    }
    );
    spin();
    check(level() == -1, "foreign parent device never leaks battery");
    mock.changed(
        svc,
        serviceI,
        {
            {"Device", QVariant::fromValue(QDBusObjectPath(dev))}
    }
    );
    spin();
    mock.holdRead = true;
    mock.changed(
        dev,
        deviceI,
        {
            {"Connected", false}
    }
    );
    spin();
    check(level() == -1, "BAS disconnect clears value");
    mock.changed(
        dev,
        deviceI,
        {
            {"Connected", true}
    }
    );
    spin();
    check(
        level() == -1 && mock.reads == 2 && mock.heldReads.size() == 1,
        "BAS reconnect clears stale cached Value and rereads"
    );
    mock.changed(
        dev,
        deviceI,
        {
            {"Connected", false}
    }
    );
    spin();
    serviceBus.send(mock.heldReads.takeFirst().createReply(QByteArray(1, char(99))));
    spin();
    check(level() == -1, "late ReadValue cannot resurrect disconnected battery");
    mock.holdRead = false;
    mock.value    = QByteArray(1, char(31));
    mock.changed(
        dev,
        deviceI,
        {
            {"Connected", true}
    }
    );
    spin();
    check(level() == 31, "next reconnect accepts fresh GATT result");
    mock.removed(chr, {charI});
    spin();
    check(level() == -1, "characteristic removal clears fallback");
    mock.added(
        dev,
        {
            {deviceI,  {{"Address", addr}, {"Connected", true}}},
            {batteryI, {{"Percentage", uchar(78)}}             }
    }
    );
    spin();
    check(level() == 78, "late Battery1 interface takes precedence");
    serviceBus.unregisterService("org.bluez");
    spin();
    check(level() == -1, "BlueZ owner loss clears values");
    mock.objects.clear();
    mock.objects[QDBusObjectPath(dev)] = {
        {deviceI,  {{"Address", addr}, {"Connected", true}}},
        {batteryI, {{"Percentage", uchar(69)}}             }
    };
    serviceBus.registerService("org.bluez");
    spin();
    check(level() == 69, "BlueZ restart snapshots existing connection");
    mock.holdSnapshot = true;
    serviceBus.unregisterService("org.bluez");
    spin();
    serviceBus.registerService("org.bluez");
    spin();
    check(!mock.heldSnapshots.isEmpty(), "snapshot request held");
    const auto staleSnapshot = mock.objects;
    mock.changed(
        dev,
        deviceI,
        {
            {"Connected", false}
    }
    );
    spin();
    for (const auto& request : mock.heldSnapshots)
        serviceBus.send(request.createReply(QVariant::fromValue(staleSnapshot)));
    mock.heldSnapshots.clear();
    spin();
    check(level() == -1, "queued disconnect wins over startup snapshot");
    mock.holdSnapshot = false;
    qInfo() << "ALL BLUETOOTH BATTERY DBUS SMOKE CHECKS PASSED";
}
