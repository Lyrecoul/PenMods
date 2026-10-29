// SPDX-License-Identifier: GPL-3.0-only
#include "helper/BluetoothStorage.h"

#include "common/Event.h"

#include <QFile>
#include <QProcess>
#include <QQmlContext>
#include <QQuickView>
#include <QSaveFile>

namespace mod {
namespace {

// S21 mounts /userdata; S36/S40/S50 may start Bluetooth. Mount before any of them.
constexpr auto kInitPath   = "/etc/init.d/S22PenModsBluetooth";
constexpr auto kInitScript = R"SH(#!/bin/sh
# Managed by PenMods. BlueZ's default /var/lib/bluetooth is otherwise on tmpfs.
set -eu
case "${1:-start}" in
    start) ;;
    stop) exit 0 ;; # Keep the store mounted while bluetoothd shuts down.
    *) exit 1 ;;
esac
umask 077
store=/userdata/PenMods/bluetooth
target=/var/lib/bluetooth
# Never write pairing keys into a RAM-backed fallback when /userdata failed to mount.
mountpoint -q /userdata || exit 1
mkdir -p "$target"
case "$(stat -f -c %T "$target")" in
    tmpfs|ramfs) ;;
    *) exit 0 ;; # Already persistent (including our existing bind mount).
esac
mkdir -p /userdata/PenMods
# Serialize boot/runtime installation; a failed attempt may be retried.
lock=/tmp/penmods-bluetooth-storage.lock
mkdir "$lock" || exit 1
stage=
trap '[ -z "$stage" ] || rm -rf "$stage"; rmdir "$lock"' EXIT
if [ ! -e "$store" ]; then
    stage=$(mktemp -d /userdata/PenMods/.bluetooth-XXXXXX)
    cp -a "$target"/. "$stage"/
    chmod 700 "$stage"
    # Publish only a complete copy. Interrupted copies never become the live store.
    mv "$stage" "$store"
    stage=
fi
[ -d "$store" ] && [ ! -L "$store" ] || exit 1
chmod 700 "$store"
mount --bind "$store" "$target"
echo 'PenMods BluetoothStorage: persistent BlueZ storage mounted'
)SH";

} // namespace

BluetoothStorage::BluetoothStorage() : Logger("BluetoothStorage") {
    QObject::connect(&Event::getInstance(), &Event::beforeUiInitialization, [this]() { install(); });
}

void BluetoothStorage::install() {
#ifdef PL_QEMU
    return;
#endif
    QFile            current(QString::fromLatin1(kInitPath));
    const QByteArray script(kInitScript);
    const bool       matches = current.open(QIODevice::ReadOnly) && current.readAll() == script;
    current.close();
    if (!matches) {
        auto writeScript = [&]() {
            QSaveFile file(QString::fromLatin1(kInitPath));
            return file.open(QIODevice::WriteOnly) && file.write(script) == script.size()
                && file.setPermissions(
                    QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup
                    | QFile::ReadOther | QFile::ExeOther
                )
                && file.commit();
        };
        if (!writeScript()) {
            // Some firmware mounts the rootfs read-only until uiCompleted.
            QProcess::execute(QStringLiteral("/bin/mount"), {"-o", "remount,rw", "/"});
            if (!writeScript()) {
                error("Cannot install {}", kInitPath);
                return;
            }
        }
        info("Installed early-boot Bluetooth storage mount");
    }

    // First installation preserves the current boot's pairings immediately. Later boots
    // mount before bluetoothd reads its database. Never restart the daemon from a UI hook.
    const int result = QProcess::execute(QStringLiteral("/bin/sh"), {QString::fromLatin1(kInitPath), "start"});
    if (result != 0) {
        error("Bluetooth persistence setup failed (exit {}); original storage left available", result);
    } else {
        info("Bluetooth storage check completed");
    }
}

} // namespace mod
