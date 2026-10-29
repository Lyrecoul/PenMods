#!/bin/sh
# Run on the pen after pushing this file, e.g.:
# adb shell 'sh /tmp/penmods-check-bluetooth.sh'
# Does not print link keys or other pairing secrets.
set -eu
store=/userdata/PenMods/bluetooth
target=/var/lib/bluetooth
fs=$(stat -f -c %T "$target")
case "$fs" in
    tmpfs|ramfs) echo "FAIL: BlueZ records are on volatile $fs"; exit 1 ;;
esac
[ "$(stat -c '%d:%i' "$target")" = "$(stat -c '%d:%i' "$store")" ] || {
    echo 'FAIL: BlueZ is not using the persistent store'; exit 1;
}
[ -x /etc/init.d/S22PenModsBluetooth ] || {
    echo 'FAIL: early-boot mount script is missing'; exit 1;
}
echo "PASS: BlueZ uses persistent storage ($fs), early-boot script installed"
