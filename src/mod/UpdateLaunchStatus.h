// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QFile>
#include <QFileInfo>

namespace mod {
// Check the actual next-start invocation even with an older helper that only
// reports the version store as installed. File presence alone is insufficient.
inline bool updateLaunchHookReady(const QString& launcher = "/usr/bin/runDictPen") {
    const QFileInfo info(launcher);
    if (!info.isFile() || info.isSymLink()) return false;
    QFile file(launcher);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray original = "stdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1";
    const QByteArray managed = "stdbuf -o0 /userdata/PenMods/penmods-update run -- ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1";
    int originals = 0, hooks = 0;
    for (const auto& line : file.readAll().split('\n')) {
        originals += line == original;
        hooks += line == managed;
    }
    return originals == 0 && hooks == 1;
}
} // namespace mod
