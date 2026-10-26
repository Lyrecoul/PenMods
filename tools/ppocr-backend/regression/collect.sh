#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# 采集多行扫描诊断现场：app 日志 + PP-OCR shim 日志 + 配置快照 + 结果页截图。
# 用法:
#   sh collect.sh [输出目录]          # 默认 build/diag/scan/collected
#   sh collect.sh out --screenshot    # 额外抓一张当前屏幕（/tmp/penmods_shot 通道）
#
# 只读采集，不重启 app、不改设备状态。

set -e
OUT=build/diag/scan/collected
if [ "${1:-}" != "" ] && [ "${1:-}" != "--screenshot" ]; then OUT=$1; fi
mkdir -p "$OUT"

if ! adb get-serialno >/dev/null 2>&1; then
    echo "adb 不可用" >&2
    exit 1
fi
SERIAL=$(adb get-serialno)
echo "device: $SERIAL"

# 1. 当前 app 日志（app 每次启动会删除旧日志，所以只取最新一份）
APP_LOG=$(adb shell 'ls -t /userdata/applog/DictPen_*.log 2>/dev/null | head -1' | tr -d '\r')
if [ -n "$APP_LOG" ]; then
    echo "app log: $APP_LOG"
    adb pull "$APP_LOG" "$OUT/" >/dev/null
    adb shell 'ls -t /userdata/applog/YD_PEN_APP.log 2>/dev/null | head -1' >/dev/null
else
    echo "warning: 设备上没有 DictPen_*.log" >&2
fi

# 2. PP-OCR shim 日志（未启用 shim 时不存在）
if adb shell '[ -f /userdisk/ppocr_shim.log ]' 2>/dev/null; then
    adb pull /userdisk/ppocr_shim.log "$OUT/" >/dev/null
fi

# 3. 配置/环境快照
{
    echo "== date =="; adb shell date
    echo "== pid =="; adb shell 'pidof YoudaoDictPen'
    echo "== ppocr enabled (shadow lib) =="; adb shell 'ls -l /userdisk/Qtlib/libyocr.so 2>/dev/null || echo off'
    echo "== ppocr target_h =="; adb shell 'cat /userdisk/ppocr_target_h 2>/dev/null'
    echo "== continuescan =="; adb shell 'grep -i continuescan /userdata/DictPenData/NeteaseYoudao/YoudaoDictPen.conf 2>/dev/null'
    echo "== penmods ai.auto_send_scan =="; adb shell 'grep -A3 -i "auto_send_scan" /userdata/PenModsconfig.json 2>/dev/null'
    echo "== libPenMods sha256 =="; adb shell 'sha256sum /userdata/PenMods/libPenMods.so 2>/dev/null'
} > "$OUT/env.txt" 2>&1
echo "env -> $OUT/env.txt"

# 4. 截图（PenMods 截屏通道；需要 /tmp/penmods_shot 触发，设备侧 1-2s 后出图）
if [ "$2" = "--screenshot" ] || [ "$1" = "--screenshot" ]; then
    adb shell 'rm -f /tmp/penmods_shot-*.png /tmp/penmods_shot.log; touch /tmp/penmods_shot'
    sleep 2
    adb pull /tmp/penmods_shot.log "$OUT/" >/dev/null 2>&1 || true
    for f in $(adb shell 'ls /tmp/penmods_shot-*.png 2>/dev/null' | tr -d '\r'); do
        adb pull "$f" "$OUT/" >/dev/null
    done
    ls -1 "$OUT"/penmods_shot-*.png 2>/dev/null || echo "warning: 没有拿到截图（检查 PenMods 截屏通道）" >&2
fi

echo "collected -> $OUT"
ls -1 "$OUT"
