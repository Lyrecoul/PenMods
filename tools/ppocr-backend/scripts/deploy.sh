#!/bin/sh
# Manage the experimental PP-OCR recognition backend on a device.
#
#   deploy.sh on [target_h]   install models + shim, restart the app (default target_h 48)
#   deploy.sh off             remove the shim, restart the app (back to the vendor engine)
#   deploy.sh h <n>           change target height (16..64), takes effect on the next line
#   deploy.sh status          what is installed / loaded right now
#   deploy.sh log             tail the shim log
#
# HOW THE SWAP WORKS
#   /usr/bin/runDictPen sets LD_LIBRARY_PATH=/userdisk/Qtlib/:...:/oem/YoudaoDictPen/output/libs,
#   and libYoudaoStitch.so has DT_NEEDED libyocr.so. Because /userdisk/Qtlib comes first, a
#   libyocr.so placed there shadows the vendor one in /oem/.../aarch64_libs. /userdisk/Qtlib
#   starts out non-existent, so nothing else is shadowed, and removing the file reverts to the
#   vendor engine. The vendor library itself is never modified.
#
# WHY THE PUSH IS NOT A PLAIN `adb push` ONTO THE TARGET
#   The running app has the shim mapped. Overwriting that file in place mutates pages of a live
#   mapping, so the app executes mismatched code and segfaults; repeated crashes make the
#   firmware re-arm the misc BCB and reboot into the recovery ramdisk. Always push to a scratch
#   name and `mv` over the target -- the rename leaves the running process on the old inode.

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$here/build
CACHE=${CACHE:-$here/.cache}

SHIM_SRC=$BUILD/libyocr.so
MODEL_SRC=$CACHE/models
DEV_SHIM=/userdisk/Qtlib/libyocr.so
DEV_MODELS=/userdisk/ppocr_models
DEV_H=/userdisk/ppocr_target_h
DEV_LOG=/userdisk/ppocr_shim.log
VENDOR_LIB=/oem/YoudaoDictPen/output/aarch64_libs/libyocr.so
APP=/oem/YoudaoDictPen/output/YoudaoDictPen
APP_LIBS=/oem/YoudaoDictPen/output/libs
SHA_TS=$(date +%s)

adb_shell() { adb shell "$@"; }

restart_app() {
    adb_shell 'sync; killall YoudaoDictPen' >/dev/null 2>&1 || true
    printf 'waiting for the app to come back'
    i=0
    while [ $i -lt 30 ]; do
        sleep 2
        pid=$(adb_shell 'pidof YoudaoDictPen' 2>/dev/null | tr -d '\r' | awk '{print $1}')
        [ -n "$pid" ] && { echo " ok (pid $pid)"; return 0; }
        printf '.'
        i=$((i + 1))
    done
    echo
    echo "warning: the app did not come back within 60s -- check"
    echo "  adb shell 'ls -t /userdata/applog/DictPen_*.log | head -1' and read the tail"
    return 1
}

case "${1:-status}" in
on)
    H=${2:-48}
    [ -s "$SHIM_SRC" ] || { echo "error: $SHIM_SRC missing, run scripts/build.sh"; exit 1; }
    [ -s "$MODEL_SRC/PP_OCRv5_mobile_rec.ncnn.param" ] || { echo "error: run scripts/fetch-deps.sh"; exit 1; }

    echo "==> models -> $DEV_MODELS"
    adb_shell "mkdir -p $DEV_MODELS /userdisk/Qtlib"
    adb push "$MODEL_SRC/PP_OCRv5_mobile_rec.ncnn.param" "$MODEL_SRC/PP_OCRv5_mobile_rec.ncnn.bin" "$DEV_MODELS/" >/dev/null

    echo "==> shim -> $DEV_SHIM (atomic replace)"
    adb push "$SHIM_SRC" "$DEV_SHIM.tmp.$SHA_TS" >/dev/null
    adb_shell "mv -f $DEV_SHIM.tmp.$SHA_TS $DEV_SHIM; sync"
    adb_shell "echo $H > $DEV_H"

    # Pre-flight with the dynamic loader: proves libyocr.so resolves to our file and that every
    # dependency is present *before* the app is restarted into it.
    echo "==> loader pre-flight"
    resolved=$(adb_shell "cd /oem/YoudaoDictPen/output && LD_LIBRARY_PATH=/userdisk/Qtlib:$APP_LIBS LD_TRACE_LOADED_OBJECTS=1 ./YoudaoDictPen 2>&1 | grep -m1 libyocr" | tr -d '\r')
    echo "    $resolved"
    case "$resolved" in
    *"$DEV_SHIM"*) ;;
    *) echo "error: libyocr.so does not resolve to $DEV_SHIM -- NOT restarting the app"; exit 1 ;;
    esac
    missing=$(adb_shell "cd /oem/YoudaoDictPen/output && LD_LIBRARY_PATH=/userdisk/Qtlib:$APP_LIBS LD_TRACE_LOADED_OBJECTS=1 ./YoudaoDictPen 2>&1 | grep -c 'not found'" | tr -d '\r')
    [ "$missing" = 0 ] || { echo "error: $missing unresolvable libraries -- NOT restarting the app"; exit 1; }

    adb_shell "rm -f $DEV_LOG"
    restart_app
    echo "==> shim log"
    adb_shell "tail -4 $DEV_LOG" | tr -d '\r'
    echo
    echo "Now scan a line. 'deploy.sh log' shows per-line timing/text; 'deploy.sh off' reverts."
    ;;
off)
    echo "==> removing $DEV_SHIM (vendor engine $VENDOR_LIB stays untouched)"
    adb_shell "rm -f $DEV_SHIM; sync"
    restart_app
    echo "==> current libyocr mapping"
    adb_shell "grep -m1 -a libyocr /proc/\$(pidof YoudaoDictPen | awk '{print \$1}')/maps | awk '{print \$6}'" | tr -d '\r'
    ;;
h)
    [ -n "$2" ] || { echo "usage: deploy.sh h <16..64>"; exit 1; }
    adb_shell "echo $2 > $DEV_H"
    echo "target height = $(adb_shell "cat $DEV_H" | tr -d '\r') (applies to the next recognized line, no restart needed)"
    ;;
status)
    echo "shim installed : $(adb_shell "[ -f $DEV_SHIM ] && echo yes || echo no" | tr -d '\r')"
    echo "target height  : $(adb_shell "cat $DEV_H 2>/dev/null || echo '(default 32)'" | tr -d '\r')"
    echo "models         : $(adb_shell "ls $DEV_MODELS 2>/dev/null | tr '\n' ' '" | tr -d '\r')"
    pid=$(adb_shell 'pidof YoudaoDictPen' 2>/dev/null | tr -d '\r' | awk '{print $1}')
    if [ -n "$pid" ]; then
        echo "app pid        : $pid"
        echo "libyocr loaded : $(adb_shell "grep -m1 -a libyocr /proc/$pid/maps | awk '{print \$6}'" | tr -d '\r')"
    else
        echo "app pid        : (not running)"
    fi
    echo "--- shim log (tail) ---"
    adb_shell "tail -6 $DEV_LOG 2>/dev/null" | tr -d '\r'
    ;;
log)
    adb_shell "tail -${2:-20} $DEV_LOG" | tr -d '\r'
    ;;
*)
    sed -n '2,9p' "$0"
    exit 1
    ;;
esac
