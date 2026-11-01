#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
device_dir=$(adb shell 'mktemp -d /tmp/penmods-issue28.XXXXXX' | tr -d '\r')
case "$device_dir" in
    /tmp/penmods-issue28.??????) ;;
    *) printf 'Unexpected device directory: %s\n' "$device_dir" >&2; exit 1 ;;
esac
trap 'adb shell "rm -rf -- $device_dir" >/dev/null' EXIT HUP INT TERM
adb push "$root/tools/external-player-test/." "$device_dir/" >/dev/null
adb push "$root/resource/models/YDP02X/qml/audiopages/ExternalPlayer.qml" "$device_dir/audiopages/ExternalPlayer.qml" >/dev/null
output=$(adb shell "QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software timeout 15 qmlscene -I $device_dir/imports $device_dir/Test.qml" 2>&1)
printf '%s\n' "$output"
case "$output" in
    *'Binding loop detected'*|*'qml: FAIL:'*) exit 1 ;;
    *'qml: PASS: repeated failed loads expose return control and correct mpv result'*) ;;
    *) exit 1 ;;
esac
output=$(adb shell "QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software timeout 15 qmlscene $device_dir/BindingTest.qml" 2>&1)
printf '%s\n' "$output"
case "$output" in
    *'qml: FAIL:'*|*'TypeError:'*) exit 1 ;;
    *'qml: PASS: optional sampling binding'*) ;;
    *) exit 1 ;;
esac
