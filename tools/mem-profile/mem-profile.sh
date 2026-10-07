#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# mem-profile.sh —— 词典笔上给宿主进程（YoudaoDictPen）拍一张内存画像。
#
#   mem-profile.sh [-p 进程名] [-m 模块名] [-w 秒] [-n 次数] [-d 设备]
#
#     -p  YoudaoDictPen   宿主进程名（pidof 取第一个）
#     -m  ffmpegplayer    要单独统计映射的模块名（子串匹配 /proc/<pid>/maps）
#     -w  0               大于 0 时进入 watch 模式：每隔 N 秒打一行，用于手/注入器驱动界面时抓峰值
#     -n  0               watch 模式最多打印多少行（0 = 无限）
#     -d                  adb 设备序列号（本地跑只要不传就是直连）
#
# 只依赖 busybox（设备上没有 python / gawk），十六进制一律用 shell 的 $((0x…)) 算。
#
# 输出一行：RSS / 线程数 / dma-buf 个数与总量 / 指定模块的映射大小 / rw 段与匿名 rw 段数。
# 判读方式见 README.md：dma-buf 与线程数应当回到基线；RSS 会留下一次性开销，
# 后续每次开关的残留应当衰减并收敛（否则就是泄漏）。

set -u

PROC="YoudaoDictPen"
MODULE="ffmpegplayer"
WATCH=0
COUNT=0
ADB_DEVICE=""

while [ $# -gt 0 ]; do
    case "$1" in
        -p) PROC=$2; shift 2 ;;
        -m) MODULE=$2; shift 2 ;;
        -w) WATCH=$2; shift 2 ;;
        -n) COUNT=$2; shift 2 ;;
        -d) ADB_DEVICE=$2; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

run() {
    if [ -n "$ADB_DEVICE" ]; then
        adb -s "$ADB_DEVICE" shell "$1"
    else
        sh -c "$1"
    fi
}

SNAPSHOT='
P=$(pidof '"$PROC"' | cut -d" " -f1)
[ -z "$P" ] && { echo "'"$PROC"' 未运行"; exit 1; }
[ -d /proc/$P ] || { echo "pid $P 已退出"; exit 1; }

RSS=$(awk "/VmRSS/{print \$2}" /proc/$P/status)
THR=$(awk "/Threads/{print \$2}" /proc/$P/status)

# dma-buf（MPP/DRM 帧池）：bufinfo 里每个对象以 8 位十六进制 size 开头
DB_N=0; DB_B=0
if [ -r /sys/kernel/debug/dma_buf/bufinfo ]; then
    for H in $(grep -E "^[0-9a-f]{8}" /sys/kernel/debug/dma_buf/bufinfo | cut -f1); do
        DB_N=$((DB_N + 1))
        DB_B=$((DB_B + 0x$H))
    done
fi

# 指定模块的映射大小
MOD_K=0
for R in $(grep "'"$MODULE"'" /proc/$P/maps 2>/dev/null | awk "{print \$1}"); do
    A=${R%-*}; B=${R#*-}
    MOD_K=$((MOD_K + (0x$B - 0x$A) / 1024))
done

RW=$(grep -c "rw-p" /proc/$P/maps)
ANON=$(grep -cE "^[0-9a-f]+-[0-9a-f]+ rw-p [0-9a-f]+ 00:00 0" /proc/$P/maps)

printf "pid=%s RSS=%dkB threads=%s dma_buf=%d(%.1fMB) module_mapped=%dkB rw_seg=%s anon_rw_seg=%s\n" \
       "$P" "$RSS" "$THR" "$DB_N" "$(echo "$DB_B" | awk "{printf \"%.1f\", \$1/1048576}")" "$MOD_K" "$RW" "$ANON"
'

if [ "$WATCH" -gt 0 ]; then
    I=0
    while :; do
        I=$((I + 1))
        printf "[%s] " "$(date +%H:%M:%S)"
        run "$SNAPSHOT"
        [ "$COUNT" -gt 0 ] && [ "$I" -ge "$COUNT" ] && break
        sleep "$WATCH"
    done
else
    run "$SNAPSHOT"
fi
