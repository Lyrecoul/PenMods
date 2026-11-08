// SPDX-License-Identifier: GPL-3.0-only
#include "helper/HfpBattery.h"

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QSocketNotifier>
#include <QTimer>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <sys/socket.h>
#include <unistd.h>

namespace mod {
namespace {
const QString profilePath = QStringLiteral("/com/github/penuniverse/HeadsetBattery");
const QString hfUuid      = QStringLiteral("0000111e-0000-1000-8000-00805f9b34fb");
const QString agUuid      = QStringLiteral("0000111f-0000-1000-8000-00805f9b34fb");

QDBusMessage bluezCall(const QString& path, const QString& interface, const QString& method) {
    return QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), path, interface, method);
}

bool number(const QByteArray& text, int& value, int max) {
    if (text.isEmpty()) return false;
    for (const auto c : text)
        if (c < '0' || c > '9') return false;
    bool ok = false;
    value   = text.toInt(&ok);
    return ok && value <= max;
}
} // namespace

class HfpBatteryConnection : public QObject {
public:
    int                      fd;
    QSocketNotifier *        reader, *writer;
    QByteArray               input, output;
    bool                     preciseBattery = false;
    std::function<void(int)> measured;
    std::function<void()>    closed;

    HfpBatteryConnection(int socket, QObject* parent) : QObject(parent), fd(socket) {
        reader = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        writer = new QSocketNotifier(fd, QSocketNotifier::Write, this);
        writer->setEnabled(false);
        connect(reader, &QSocketNotifier::activated, this, [this]() { receive(); });
        connect(writer, &QSocketNotifier::activated, this, [this]() { flush(); });
    }
    ~HfpBatteryConnection() override { stop(); }
    void stop() {
        reader->setEnabled(false);
        writer->setEnabled(false);
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    void fail() {
        stop();
        if (closed) closed();
    }
    void flush() {
        while (fd >= 0 && !output.isEmpty()) {
            const auto n = ::send(fd, output.constData(), output.size(), MSG_NOSIGNAL);
            if (n > 0) output.remove(0, n);
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else {
                fail();
                return;
            }
        }
        writer->setEnabled(fd >= 0 && !output.isEmpty());
    }
    void reply(const QByteArray& body = {}) {
        if (!body.isEmpty()) output += "\r\n" + body + "\r\n";
        output += "\r\nOK\r\n";
        if (output.size() > 8192) {
            fail();
            return;
        }
        flush();
    }
    void error() {
        output += "\r\nERROR\r\n";
        if (output.size() > 8192) {
            fail();
            return;
        }
        flush();
    }
    void receive() {
        char bytes[1024];
        while (fd >= 0) {
            const auto n = ::read(fd, bytes, sizeof bytes);
            if (n > 0) input.append(bytes, n);
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else {
                fail();
                return;
            }
            if (input.size() > 4096) {
                fail();
                return;
            }
            int end;
            while (fd >= 0 && (end = input.indexOf('\r')) >= 0) {
                const auto command = input.left(end).trimmed();
                input.remove(0, end + 1);
                if (!command.isEmpty()) consume(command);
            }
        }
    }
    void consume(const QByteArray& command) {
        if (command.startsWith("AT+BRSF=")) {
            // HF Indicators only. No codec negotiation, call control, or SCO.
            reply("+BRSF: 1024");
        } else if (command == "AT+CIND=?") {
            reply("+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3))");
        } else if (command == "AT+CIND?") {
            reply("+CIND: 0,0,0"); // No telephony service or calls on the pen.
        } else if (command == "AT+BIND=?") {
            reply("+BIND: (2)");
        } else if (command == "AT+BIND?") {
            reply("+BIND: 2,1");
        } else if (command.startsWith("AT+BIEV=")) {
            const auto fields = command.mid(8).split(',');
            int        indicator, level;
            if (fields.size() != 2 || !number(fields[0], indicator, 2) || indicator != 2
                || !number(fields[1], level, 100)) {
                error();
                return;
            }
            preciseBattery = true;
            if (measured) measured(level);
            reply();
        } else if (command.startsWith("AT+XAPL=")) {
            reply("+XAPL=iPhone,2");
        } else if (command.startsWith("AT+IPHONEACCEV=")) {
            const auto fields = command.mid(15).split(',');
            int        count, level = -1;
            if (!number(fields.value(0), count, 16) || count < 1 || fields.size() != 1 + 2 * count) {
                error();
                return;
            }
            for (int i = 0; i < count; ++i) {
                int key, value;
                if (!number(fields[1 + 2 * i], key, 2) || key < 1
                    || !number(fields[2 + 2 * i], value, key == 1 ? 9 : 1)) {
                    error();
                    return;
                }
                if (key == 1) level = (value + 1) * 10; // Apple deciles, not exact 0..100.
            }
            if (!preciseBattery && level >= 0 && measured) measured(level);
            reply();
        } else if (
            command.startsWith("AT+CMER=") || command.startsWith("AT+BIND=") || command.startsWith("AT+VGS=")
            || command.startsWith("AT+VGM=") || command.startsWith("AT+CLIP=") || command.startsWith("AT+CCWA=")
            || command.startsWith("AT+CMEE=") || command == "AT+COPS=3,0" || command == "AT+CNUM"
        ) {
            reply();
        } else {
            error(); // Never acknowledge call/audio operations we cannot implement.
        }
    }
};

HfpBattery::HfpBattery(const QDBusConnection& bus, QObject* parent) : QObject(parent), mBus(bus) {
    mExported = mBus.registerObject(profilePath, this, QDBusConnection::ExportAllSlots);
}
HfpBattery::~HfpBattery() {
    ++mEpoch;
    qDeleteAll(mConnections);
    if (mRegistered) {
        auto request = bluezCall(
            QStringLiteral("/org/bluez"),
            QStringLiteral("org.bluez.ProfileManager1"),
            QStringLiteral("UnregisterProfile")
        );
        request << QVariant::fromValue(QDBusObjectPath(profilePath));
        mBus.asyncCall(request);
    }
    if (mExported) mBus.unregisterObject(profilePath);
}
void HfpBattery::reset() {
    const auto epoch   = ++mEpoch;
    const auto devices = mConnections.keys();
    for (const auto& device : devices) closeDevice(device);
    mDevices.clear();
    mAttempted.clear();
    mRegistered = false;
    mOwner.clear();
    if (!mExported) return;
    auto request = bluezCall(
        QStringLiteral("/org/bluez"),
        QStringLiteral("org.bluez.ProfileManager1"),
        QStringLiteral("RegisterProfile")
    );
    request << QVariant::fromValue(QDBusObjectPath(profilePath)) << agUuid
            << QVariantMap{
                   {QStringLiteral("Name"),                  QStringLiteral("PenMods headphone battery")},
                   {QStringLiteral("Channel"),               QVariant::fromValue(quint16(0))            },
                   {QStringLiteral("Version"),               QVariant::fromValue(quint16(0x0107))       },
                   {QStringLiteral("Features"),              QVariant::fromValue(quint16(0))            },
                   {QStringLiteral("RequireAuthentication"), true                                       },
                   {QStringLiteral("RequireAuthorization"),  false                                      },
                   {QStringLiteral("AutoConnect"),           false                                      }
    };
    auto* watcher = new QDBusPendingCallWatcher(mBus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, epoch](QDBusPendingCallWatcher* pending) {
        QDBusPendingReply<> reply = *pending;
        pending->deleteLater();
        if (epoch != mEpoch || reply.isError()) return;
        mRegistered = true;
        mOwner      = pending->reply().service();
        for (const auto& device : mDevices) connectDevice(device);
    });
}
void HfpBattery::sync(const QSet<QString>& devices) {
    const auto removed = mDevices - devices;
    mDevices           = devices;
    for (const auto& device : removed) {
        mAttempted.remove(device);
        closeDevice(device);
    }
    if (mRegistered)
        for (const auto& device : mDevices) connectDevice(device);
}
void HfpBattery::connectDevice(const QString& device) {
    if (mAttempted.contains(device) || mConnections.contains(device)) return;
    mAttempted.insert(device);
    auto request = bluezCall(device, QStringLiteral("org.bluez.Device1"), QStringLiteral("ConnectProfile"));
    request << hfUuid;
    auto* watcher = new QDBusPendingCallWatcher(mBus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, &QObject::deleteLater);
}
void HfpBattery::closeDevice(const QString& device) {
    auto* connection = mConnections.take(device);
    if (!connection) return;
    connection->stop();
    connection->deleteLater();
    emit batteryChanged(device, -1);
}
void HfpBattery::Release() {
    if (!fromBluez()) return;
    ++mEpoch;
    mRegistered        = false;
    const auto devices = mConnections.keys();
    for (const auto& device : devices) closeDevice(device);
}
void HfpBattery::NewConnection(
    const QDBusObjectPath&         device,
    const QDBusUnixFileDescriptor& descriptor,
    const QVariantMap&
) {
    const auto path = device.path();
    if (!fromBluez()) return;
    if (!mDevices.contains(path)) {
        sendErrorReply(QStringLiteral("org.bluez.Error.Rejected"), QStringLiteral("Not a connected headset"));
        return;
    }
    const int fd = fcntl(descriptor.fileDescriptor(), F_DUPFD_CLOEXEC, 0);
    if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        if (fd >= 0) ::close(fd);
        sendErrorReply(QStringLiteral("org.bluez.Error.Rejected"), QStringLiteral("Cannot own RFCOMM socket"));
        return;
    }
    closeDevice(path);
    auto* connection = new HfpBatteryConnection(fd, this);
    mConnections.insert(path, connection);
    connection->measured = [this, path](int level) { emit batteryChanged(path, level); };
    connection->closed   = [this, path]() { closeDevice(path); };
}
bool HfpBattery::fromBluez() const {
    if (calledFromDBus() && !mOwner.isEmpty() && message().service() == mOwner) return true;
    if (calledFromDBus()) sendErrorReply(QStringLiteral("org.bluez.Error.Rejected"), QStringLiteral("BlueZ only"));
    return false;
}
void HfpBattery::RequestDisconnection(const QDBusObjectPath& device) {
    if (fromBluez()) closeDevice(device.path());
}
} // namespace mod
