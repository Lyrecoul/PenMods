# FFmpegPlayer

面向 RK3326 / AArch64 Linux、Qt 5.15 的 QML 播放器插件。使用项目补丁版 FFmpeg 3.4.8，支持本地文件、HTTP/HTTPS MP4、DASH `.mpd`、独立音轨、请求头、暂停与跳转、0.5–2 倍变速不变调、按住临时 2 倍速，以及外挂 ASS/LRC 字幕。

H.264、HEVC、VP8 优先尝试 Rockchip MPP；初始化失败或运行中检测到硬解故障时回退软解。Qt 5 OpenGL 场景图支持 YUV 纹理和 DRM PRIME / DMA-BUF 导入，CPU RGBA 是备用渲染路径。Qt 6 和非 Linux 平台目前不在验收范围内。

## 项目目录

| 目录 | 内容 |
| --- | --- |
| `src/` | 播放器实现：FFmpeg 输入与解码、音视频时钟和队列、ALSA 输出、视频渲染，以及 Qt/QML 插件接口。 |
| `src/qml/` | QML 模块清单；安装时与插件库一起复制。 |
| `examples/` | 宿主应用接入示例，包含播放控制、进度条和错误显示。 |
| `patches/` | 项目对 FFmpeg 3.4.8 的补丁；构建时按文件名顺序应用。 |
| `external/alsa-lib-1.1.5/` | ALSA 头文件，供播放器编译使用。 |
| `external/rkmpp/` | Rockchip MPP、DRM 头文件及 `pkg-config` 描述文件。 |
| `external/dictpen/` | 目标设备提供的 ALSA、MPP、DRM 动态库。它们必须与设备架构和 ABI 匹配，不能用于 x86_64 主机链接。 |
| `build/` | Xmake 构建与安装输出，可重新生成。 |

## 构建 ARM64 插件

需要 Xmake、Zig、主机可运行的 Qt 5 `moc`、ARM64 Qt 5.15 SDK、`patch`、`make`、`pkg-config`。项目使用 C++17；首次构建需要网络下载依赖。

以下命令从项目根目录执行。FFmpeg 的配置参数、项目补丁和静态依赖由 `xmake.lua` 中的包配方管理。

### 1. 配置交叉编译

```sh
xmake f -c -p linux -a arm64-v8a -m release \
  --toolchain=zigcc --cross=aarch64-linux-gnu.2.27 \
  --qt=/path/to/arm64-qt --ccache=n -y
```

`--qt` 指向 ARM64 Qt 5.15 SDK；其中的 `bin/moc` 必须能在构建主机上运行。`-c` 清除过期的工具检测缓存。

### 2. 编译 FFmpeg、libass 及其静态依赖

```sh
xmake require -v ffmpeg
xmake require -v libass
```

首次构建时，Xmake 会获取并编译 FFmpeg 3.4.8、OpenSSL 3、libxml2、libass 0.17.4、FreeType、HarfBuzz 和 FriBidi，后续构建复用缓存。构建过程中应用 `patches/` 中的补丁，并将库安装到 Xmake 包缓存。若 `external/dictpen/` 中存在设备版 `librockchip_mpp.so.1` 和 `libdrm.so.2`，FFmpeg 同时启用 Rockchip MPP 解码；否则构建软解版本。

### 3. 编译 QML 插件

```sh
xmake -v -j4 ffmpegplayerplugin
```

### 4. 安装插件

```sh
xmake install -o build/dist-static
```

输出目录：

```text
build/dist-static/
  qml/FFmpegPlayer/libffmpegplayerplugin.so
  qml/FFmpegPlayer/qmldir
```

FFmpeg、OpenSSL 3、libxml2 和字幕渲染依赖静态链接进 `libffmpegplayerplugin.so`，无需附带它们的 `.so`。Qt、ALSA、MPP、DRM 和图形驱动继续使用设备兼容版本。复制整个目录后，设置：

```sh
export QML2_IMPORT_PATH=/path/to/ffmpegplayer/qml
```

插件通过 `src/ffmpegplayer.exports` 只导出 Qt 插件入口，静态依赖的符号均为本地符号。即使宿主已加载系统原版 FFmpeg，插件内部仍使用补丁版本。

字幕依赖关闭 Fontconfig、ICU、libunibreak 等可选组件，不打包字体；部署时需提供可读取的字体文件。

目标词典笔的 Qt 库位于 `/userdisk/Qtlib`，图形平台为 Wayland，shell 集成为 `xdg-shell-v6`。在该设备上独立运行时设置以下变量，其他设备应按实际环境调整：

```sh
export QT_QPA_PLATFORM=wayland
export QT_WAYLAND_SHELL_INTEGRATION=xdg-shell-v6
export XDG_RUNTIME_DIR=/var/run
```

## QML 接入

```qml
import QtQuick 2.15
import FFmpegPlayer 1.0

VideoPlayer {
    id: player
    anchors.fill: parent
    onErrorOccurred: console.warn(error)
    Component.onCompleted: player.open(
        "https://example.org/video.mp4",
        "https://example.org/audio.m4a",
        { "Referer": "https://example.org/" })
}
```

合流文件或 MPD 的第二个参数传空字符串即可。`examples/Player.qml` 提供悬浮播放控件、字幕开关、倍速和缩放/平移示例。宿主传入 `videoUrl`、可选 `audioUrl` 与 `headers`；创建后更新这些属性会重新打开媒体，同一轮事件中的更新会合并。清空 `videoUrl` 或关闭窗口会停止播放。修改请求头时需重新赋值整个 `headers` 对象。`displayTitle` 可覆盖自动提取的媒体名。

常用接口：

| 接口 | 行为 |
| --- | --- |
| `open(video, audio, headers)` | 原子设置输入并异步打开 |
| `play()` / `pause()` / `stop()` | 播放、暂停、释放资源；EOF 后 `play()` 从头重播 |
| `seek(seconds)` | 按秒跳转，连续请求以最后一次为准，暂停状态保持暂停 |
| `seekForward(seconds)` / `seekBackward(seconds)` | 相对跳转，默认 10 秒 |
| `playbackRate` | 0.5–2.0，改变速率会清空旧音频并重新定位 |
| `boostRate` | 长按倍率，默认 2.0，范围 0.5–2.0，与 `playbackRate` 独立 |
| `beginBoost()` / `endBoost()` | 临时使用 `boostRate`，结束时固定恢复 1.0 倍速 |
| `source` / `audioSource` / `httpHeaders` | 输入与 HTTP 请求头；`autoPlay` 默认关闭 |
| `loading` / `seeking` / `errorString` | 异步状态与错误 |
| `position` / `duration` | 秒；未知时长为 0 |
| `videoDecoder` / `hardwareDecoding` | 当前解码器及硬解状态 |

不要使用播放状态的数字值；用 `VideoPlayer.PlayingState` 等枚举。错误通过 `errorOccurred` 通知，媒体结束通过 `endOfMedia` 通知。

### 手势与底边进度条

`examples/Player.qml` 在未缩放时支持左右滑动：向右快进、向左后退，滑过整个画面宽度对应 60 秒，可用 `seekSecondsPerWidth` 调整。滑动时左上角显示“预览时间/总时长”，两处进度条同步预览；松手后跳转，取消手势则不跳转，暂停状态保持暂停。

长按时使用独立的 `holdPlaybackRate`（默认 2.0），左上角仅在长按期间显示倍率，松手恢复 1.0。控制栏选速不会显示该提示，也不会改变长按倍率。例如控制栏选 1.5 倍后，长按为 2 倍，松手为 1 倍。

插件可在 `VideoPlayer` 区域底边绘制进度条，默认关闭；示例默认开启，缩放时自动隐藏。颜色支持透明度，高度以 QML 逻辑像素计：

```qml
VideoPlayer {
    progressBarEnabled: true
    progressBarColor: "#80cfff"
    progressBarBackgroundColor: "transparent"
    progressBarHeight: 2
    boostRate: 2.0
}
```

| 插件属性 | 示例窗口属性 | 说明 |
| --- | --- | --- |
| `progressBarEnabled` | `edgeProgressEnabled` | 进度条开关；示例同时根据缩放状态控制显示 |
| `progressBarColor` | `edgeProgressColor` | 已播放部分颜色，默认淡蓝色 `#80cfff` |
| `progressBarBackgroundColor` | `edgeProgressBackgroundColor` | 未播放部分颜色，默认透明 |
| `progressBarHeight` | `edgeProgressHeight` | 默认 2，设为 0 隐藏 |
| `boostRate` | `holdPlaybackRate` | 长按倍率，默认 2.0 |

`progressBarPosition` 默认 -1，自动跟随播放进度；设为非负秒数可显示跳转预览，不会改变实际播放位置。宿主自定义缩放时应自行绑定 `progressBarEnabled`，示例已处理。未知时长或停止播放时不显示进度条。

### 外挂 ASS/LRC 字幕

字幕由 libass 渲染为独立透明图层，跟随播放器时钟，支持暂停、跳转、重播和倍速。

```qml
VideoPlayer {
    id: player
    source: "file:///userdisk/Music/movie.mp4"
    subtitleSource: "file:///userdisk/Music/movie.ass"
    autoPlay: true
    subtitlesEnabled: true
    subtitleDelay: 0.0
    subtitleFontScale: 1.0
}
```

播放途中可随时关闭或恢复字幕，不影响音视频播放；恢复后显示当前播放进度对应的字幕：

```javascript
player.subtitlesEnabled = false // 隐藏，保留字幕内容
player.subtitlesEnabled = true  // 恢复显示
player.clearSubtitles()         // 清除内容，需重新加载才能显示
```

换成 .lrc 文件即可显示整行歌词。支持一行多个时间戳、同时间戳双语合并、空行清屏和 offset 标签；offset 为正时歌词提前（毫秒），subtitleDelay 为正时字幕延后（秒）。增强 LRC 的逐字标签只被移除，当前不做逐字高亮。最后一行会保留在结束后的暂停画面中，停止时隐藏；LRC 也可以用最后一个空时间标签清屏。

| API | 说明 |
| --- | --- |
| subtitleSource | 本地文件或 qrc URL；异步读取，最多 4 MiB |
| subtitlesEnabled | 字幕显示开关，默认 true |
| subtitleDelay | 延迟秒数，正数延后；暂停时也可调整 |
| subtitleFontFile | 本地字体文件 URL；空值时自动探测 |
| subtitleFontScale | 字体缩放，范围 0.25–4，默认 1 |
| subtitleLoading | 加载中 |
| subtitleErrorString | 字幕错误，与视频 errorString 独立 |
| setSubtitleText(text, format) | 提交宿主已下载的文本，format 为 ass 或 lrc；后台解析 |
| clearSubtitles() | 取消待处理加载并清除字幕 |

文本支持 UTF-8、BOM 编码和 GB18030 回退；LRC 最多 10000 个时间戳。ASS 样式、颜色和定位交给 libass，字幕格式错误不会停止视频。网络字幕由宿主下载后调用 setSubtitleText，本版不直接请求远程字幕 URL，也不支持视频内嵌字幕轨。

字幕源与视频源独立：`stop()` 隐藏字幕并保留内容供重播；切换视频时，宿主应同时更新 `subtitleSource` 或调用 `clearSubtitles()`，避免沿用上一段视频的字幕。`examples/Player.qml` 通过 `subtitleUrl` 和 `subtitleFontUrl` 接入字幕和字体，底部“字幕”按钮可在播放途中切换显示，宿主也可设置示例窗口的 `subtitlesEnabled` 属性。

字体默认优先使用设备 /usr/lib/fonts/NotoSansSC-Regular.otf，可通过 subtitleFontFile 或 FFPLAYER_SUBTITLE_FONT 指定其他字体。本版使用单个回退字体，不扫描全部系统字体，也不提取 ASS 内嵌字体；字体缺失时通过 subtitleErrorString 报告。图层跟随视频的宽高比、旋转和 QML 变换。

### 设备音频锁

打开 ALSA 前，播放器会在 `/tmp/audio_wakelocks` 创建独立的 `ffmpegplayer-<pid>-*.lock` 文件，内容为当前 PID；关闭输出及初始化失败时释放。多播放器实例分别持锁。普通 Linux 主机没有该目录时跳过；设置 `FFPLAYER_AUDIO_LOCK_DIR` 可指定其他目录，此时目录必须存在且可写。进程被强制杀死时可能留下文件，设备服务应按 PID 是否存活判断锁有效性。

### 运行开关

| 环境变量 | 用途 |
| --- | --- |
| `FFPLAYER_HWDEC=0` | 强制软解；默认自动尝试硬解 |
| `FFPLAYER_HWDEC=force` | 调试时跳过硬解能力预检 |
| `FFPLAYER_ZEROCOPY=0` | 禁用 DMA-BUF 导入，保留其他渲染路径 |
| `FFPLAYER_GL=0` | 强制 CPU 转 RGBA |
| `SSL_CERT_FILE` / `SSL_CERT_DIR` | 可选 HTTPS CA 文件 / 哈希证书目录；未设置时使用 OpenSSL 默认信任路径及设备 `/etc/ssl/certs`，不要求 CA 合集文件。显式配置加载失败时报错，始终校验证书链和主机名。 |

HTTPS 默认验证证书链和主机名。设备系统时间和 CA 文件须正确。网络读写有超时及取消机制，尚不提供应用层自动重连、内嵌字幕轨选择或播放列表管理。
