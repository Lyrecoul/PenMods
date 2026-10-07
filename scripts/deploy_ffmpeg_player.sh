#!/bin/bash
# 部署内嵌 FFmpeg 播放器插件（QML 模块）到设备。
#
#   scripts/deploy_ffmpeg_player.sh [mode]
#
# 产物来自 `xmake build ffmpegplayerplugin`：
#   build/linux/arm64-v8a/<mode>/qml/FFmpegPlayer/{libffmpegplayerplugin.so,qmldir}
#
# 注意：`.so` 先推到临时文件名再 mv，避免覆盖正在被宿主进程 mmap 的旧文件
# （直接覆盖会让运行中的进程执行到不一致的代码而崩溃，见 AGENTS.md）。

set -e

MODE=${1:-release}
ARCH=arm64-v8a
PLAT=linux
SRC_DIR="build/$PLAT/$ARCH/$MODE/qml/FFmpegPlayer"
DST_DIR=/userdata/PenMods/qml/FFmpegPlayer

if [ ! -f "$SRC_DIR/libffmpegplayerplugin.so" ]; then
    echo "找不到产物: $SRC_DIR/libffmpegplayerplugin.so"
    echo "先构建（插件的依赖链默认不参与工程解析，见 external/ffmpeg-player/UPSTREAM.md）："
    echo "  PENMODS_WITH_PLAYER=1 xmake f -c <原有配置...> && xmake build ffmpegplayerplugin"
    exit 1
fi

echo "==> 部署 $SRC_DIR/ 到 $DST_DIR/"
adb shell "mkdir -p $DST_DIR"

# qmldir 只是文本，直接覆盖
adb push "$SRC_DIR/qmldir" "$DST_DIR/qmldir"

# 插件本体：tmp + rename
adb push "$SRC_DIR/libffmpegplayerplugin.so" "$DST_DIR/libffmpegplayerplugin.tmp.so"
adb shell "mv -f $DST_DIR/libffmpegplayerplugin.tmp.so $DST_DIR/libffmpegplayerplugin.so; sync"

adb shell "ls -la $DST_DIR/"
echo
echo "==> 重启宿主进程以加载新插件"
adb shell 'killall YoudaoDictPen'
echo "完成（guardian 会在几秒内重启应用）"
