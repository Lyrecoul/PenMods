// SPDX-License-Identifier: GPL-3.0-only
#include "chatbot/MathServerRuntime.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <signal.h>
#include <unistd.h>

using mod::chatbot::MathServerRuntime;
static void check(bool value, const char* message) {
    if (!value) {
        fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}
static bool until(const std::function<bool()>& fn, int milliseconds = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        if (fn()) return true;
        QThread::msleep(5);
    }
    return fn();
}
static bool listener(quint16 port) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    return socket.waitForConnected(20);
}
static QString executable(qint64 pid) { return QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget(); }
static QString copy(const QString& target) {
    QDir().mkpath(QFileInfo(target).absolutePath());
    check(QFile::copy(QCoreApplication::applicationFilePath(), target), "copy executable fixture");
    QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return target;
}
static void finish(MathServerRuntime& runtime) {
    runtime.stop();
    check(until([&] { return runtime.processId() == 0; }), "owned shutdown");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (QFileInfo(app.applicationFilePath()).fileName() == "mathjax-server"
        || QFileInfo(app.applicationFilePath()).fileName() == "unrelated-worker") {
        if (qEnvironmentVariableIntValue("MATH_FIXTURE_IGNORE_TERM")) signal(SIGTERM, SIG_IGN);
        QTcpServer server;
        if (!server.listen(QHostAddress::Any, quint16(qEnvironmentVariableIntValue("MATH_FIXTURE_PORT")))) return 44;
        return app.exec();
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--owner-child") {
        MathServerRuntime runtime(
            app.arguments()[2],
            [](const QJsonObject&) { return true; },
            nullptr,
            quint16(qEnvironmentVariableIntValue("MATH_FIXTURE_PORT"))
        );
        runtime.load({
            {"enabled", true}
        });
        check(runtime.start(), "parent death child start");
        check(until([&] { return runtime.running(); }), "parent death running");
        fprintf(stdout, "%lld\n", static_cast<long long>(runtime.processId()));
        fflush(stdout);
        return app.exec();
    }
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory");
    QTcpServer choosePort;
    check(choosePort.listen(QHostAddress::Any, 0), "choose port");
    const auto port = choosePort.serverPort();
    choosePort.close();
    qputenv("MATH_FIXTURE_PORT", QByteArray::number(port));
    const QString     root      = temporary.path();
    const auto        managed   = copy(root + "/store/active/mathjax/mathjax-server");
    int               writes    = 0;
    bool              saveWorks = true;
    QJsonObject       durable;
    int               selections = 0;
    MathServerRuntime runtime(
        managed,
        [&](const QJsonObject& object) {
            ++writes;
            if (!saveWorks) return false;
            durable = object;
            return true;
        },
        nullptr,
        port
    );
    runtime.selectionChanged = [&] { ++selections; };
    QString failure;
    runtime.failed = [&](const QString& error) { failure = error; };
    runtime.load({
        {"enabled",        true      },
        {"server_path",    "/legacy/mathjax-server"},
        {"server_source",  "custom"  },
        {"future_setting", 17        }
    });
    check(runtime.installed() && runtime.path() == managed, "legacy custom path cannot override installed server");
    check(
        !runtime.config().contains("server_path") && !runtime.config().contains("server_source")
            && !durable.contains("server_path") && !durable.contains("server_source")
            && durable.value("future_setting").toInt() == 17,
        "legacy source keys are removed while unrelated settings survive"
    );
    saveWorks = false;
    check(!runtime.setEnabled(false) && runtime.enabled() && runtime.processId() == 0, "failed durable save preserves enabled state");
    check(runtime.status().contains("无法保存"), "save error visible in runtime status");
    saveWorks = true;
    check(runtime.start(), "managed server accepted");
    check(
        until([&] { return runtime.running() && executable(runtime.processId()) == managed && listener(port); }),
        "legacy custom config starts only installed generation binary"
    );
    const auto first = runtime.processId();
    check(runtime.update(runtime.config()) && runtime.path() == managed, "config roundtrip retains managed path");
    saveWorks = false;
    check(
        !runtime.setEnabled(false) && runtime.processId() == first && runtime.running(),
        "save failure does not stop managed owner"
    );
    saveWorks = true;
    for (int n = 0; n < 30; ++n) check(runtime.start(), "duplicate caller accepts shared process");
    check(runtime.processId() == first, "duplicate callers do not duplicate services");
    check(runtime.setEnabled(false), "disable saved");
    check(until([&] { return runtime.processId() == 0 && !listener(port); }), "disable terminates owner");

    const auto unrelatedPath = copy(root + "/unrelated-worker");
    QProcess   unrelated;
    unrelated.start(unrelatedPath, QStringList{});
    check(unrelated.waitForStarted() && until([&] { return listener(port); }), "unrelated collision fixture");
    runtime.load({
        {"enabled", true}
    });
    failure.clear();
    check(!runtime.start() && !failure.isEmpty() && runtime.processId() == 0, "unrelated port collision reported");
    check(
        selections > 0 && runtime.enabled() && runtime.path() == managed,
        "managed configuration stays observable despite launch failure"
    );
    check(unrelated.state() == QProcess::Running, "unrelated listener never killed");
    unrelated.kill();
    unrelated.waitForFinished();

    // Same name outside selected path/store is still unrelated and must survive.
    const auto sameName = copy(root + "/unrelated/mathjax-server");
    QProcess   named;
    named.start(sameName, QStringList{});
    check(named.waitForStarted() && until([&] { return listener(port); }), "unrelated same-name fixture");
    check(!runtime.start() && named.state() == QProcess::Running, "same name alone is not retirement authority");
    named.kill();
    named.waitForFinished();

    const auto stale = copy(root + "/store/objects/old-generation/mathjax-server");
    QProcess   oldManaged;
    auto       env = QProcessEnvironment::systemEnvironment();
    env.insert("MATH_FIXTURE_IGNORE_TERM", "1");
    oldManaged.setProcessEnvironment(env);
    oldManaged.start(stale, QStringList{});
    check(oldManaged.waitForStarted() && until([&] { return listener(port); }), "stale managed fixture");
    check(runtime.start(), "stale managed retirement accepted");
    check(
        until([&] { return runtime.running() && executable(runtime.processId()) == managed && listener(port); }),
        "stale managed process terminated and escalated without UI blocking"
    );
    check(oldManaged.state() == QProcess::NotRunning, "stale managed exact process gone");
    check(runtime.setEnabled(false), "disabled state saved after collision test");
    check(until([&] { return runtime.processId() == 0 && !listener(port); }), "managed server stops after disable");
    finish(runtime);

    MathServerRuntime
        absent(root + "/absent/active/mathjax/mathjax-server", [](const QJsonObject&) { return true; }, nullptr, port);
    absent.load({
        {"enabled", true},
        {"server_path", "/legacy/mathjax-server"}
    });
    check(!absent.installed() && absent.path().isEmpty() && !absent.start(), "missing managed server cannot use legacy custom path");

    QProcess owner;
    owner.start(app.applicationFilePath(), {"--owner-child", managed});
    check(owner.waitForStarted() && until([&] { return owner.canReadLine(); }), "owner fixture launched");
    const auto orphan = owner.readLine().trimmed().toLongLong();
    check(orphan > 1 && executable(orphan) == managed, "owner fixture exact child");
    owner.kill();
    owner.waitForFinished();
    check(until([&] { return !listener(port); }), "parent death kills owned server and releases port");
    fprintf(
        stdout,
        "PASS legacy-path migration, managed invocation, duplicates, collisions, exact cleanup, "
        "parent death\n"
    );
    return 0;
}
