// SPDX-License-Identifier: GPL-3.0-only
#include "mod/UpdateLaunchStatus.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir    temp;
    if (!temp.isValid() || argc != 2) return 2;
    const QString launcher = temp.path() + "/usr/bin/runDictPen";
    const QString helper   = temp.path() + "/userdata/PenMods/penmods-update";
    auto          write    = [](const QString& path, const QByteArray& text) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
    };
    const QByteArray stock = "#!/bin/sh\nstdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1\n";
    if (!write(launcher, stock) || !write(helper, "#!/bin/sh\nexit 0\n")) return 2;
    QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QJsonObject ready{
        {"launcher_ready", true}
    };
    const QJsonObject unready{
        {"launcher_ready", false}
    };
    auto check = [](bool ok, const char* description) {
        if (!ok) qCritical() << description;
        return ok;
    };
    if (!check(!mod::updateLaunchStatusReady({}, launcher), "legacy helper must reject stock hook")) return 1;
    const int result = QProcess::execute("sh", {QString::fromLocal8Bit(argv[1]), "--rootfs-prefix", temp.path()});
    if (result) return 2;
    if (!check(mod::updateLaunchStatusReady(ready, launcher), "modern helper recognizes guarded hook; UI must agree"))
        return 1;
    if (!check(!mod::updateLaunchStatusReady(unready, launcher), "explicit helper rejection must be honored")) return 1;
    if (!check(
            !mod::updateLaunchStatusReady(
                {
                    {"launcher_ready", "true"}
    },
                launcher
            ),
            "malformed readiness must fail closed"
        ))
        return 1;
    // Future hook formats are owned by the helper, not a second UI parser.
    if (!write(launcher, "#!/bin/sh\n# future helper-managed invocation\n")) return 2;
    if (!check(mod::updateLaunchStatusReady(ready, launcher), "reported readiness must survive hook format changes"))
        return 1;
    if (!write(
            launcher,
            "#!/bin/sh\nstdbuf -o0 /userdata/PenMods/penmods-update run -- ./YoudaoDictPen -platform wayland > "
            "$LOG_FILE 2>&1\n"
        ))
        return 2;
    if (!check(mod::updateLaunchStatusReady({}, launcher), "old helper without readiness retains legacy fallback"))
        return 1;
    if (!check(!mod::updateLaunchStatusReady(unready, launcher), "legacy hook cannot override helper rejection"))
        return 1;
    qInfo() << "update launcher readiness checks passed";
    return 0;
}
