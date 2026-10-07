# 内嵌 FFmpeg 播放器（Haikure/ffmpeg-player）可行性分析

> 目标: 评估把上游 QML 播放器插件 `ffmpeg-player` 并入 PenMods、替换/补充现有视频播放路径的可行性
>
> 分析日期: 2026-10-07
>
> 上游: <https://github.com/Haikure/ffmpeg-player>（v1.0.0，GPL-3.0，C++17 + xmake）
>
> 目标文件: `binary/YoudaoDictPen`、`/usr/lib/libQt5*`、设备实测（YDP02X, SN 2D90500000818624, RK3326）

---

## 0. 结论

| 问题 | 结论 |
|---|---|
| 技术路线（Qt 插件 + 静态 FFmpeg + QSG 内嵌渲染） | **可行**，与 PenMods 现有工具链/ABI 完全一致 |
| 上游主打卖点「Rockchip MPP 硬解」 | **出厂固件上不可用**：设备树 `vpu_combo` 为 `disabled`，而厂商 MPP 用户在无 VPU 时会在 `mpp_init()` 里 **SIGSEGV**（实测三条路径都会崩）。patch 内核/DTB 打开该节点后可用（维护者已有 `okay` 版 boot.img） |
| 上游另一卖点「DMA-BUF 零拷贝渲染」 | 只有硬解可用时才有意义（软解帧是 yuv420p，走纹理上传即可，GLES2 足够）；libmali 已支持 `EGL_EXT_image_dma_buf_import` |
| 真正值得要的能力 | **把视频画进 Qt 场景图**：跟随左右手 180° 旋转、画面内控件/手势、ASS/LRC 外挂字幕、倍速、HTTP/DASH/独立音轨 |
| 主要成本 | 构建集成（首次静态编译 FFmpeg+OpenSSL3+libass）、产物 10–20 MB、**进程内崩溃放大风险** |
| 主要风险 | 解码崩溃 = 宿主进程崩溃，连续崩溃会被固件升级为 `misc`/`boot-recovery`（见 AGENTS.md）；**硬解必须先探测 `/dev/vpu_service` 再用** |

建议：以上游代码为基础，**自建一个可选的独立 target/组件**（不要直接塞进 `libPenMods.so`），
按 `OcrBackend` 那种文件式开关控制、保留 mpv 兜底；硬解路径以「VPU 是否可用」为前置条件。

---

## 1. 上游组件是什么

```
ffmpeg-player/
├── src/                     全部为 header-only 实现（无 .cpp，只有插件入口）
│   ├── FFmpegVideoPlayerQml.hpp    ~65 KB  QML 类型 VideoPlayer（线程/时钟/队列/控制）
│   ├── VideoRenderNode.hpp         ~29 KB  QSG 节点：DRM_PRIME→EGLImage 或 YUV 纹理上传
│   ├── SubtitleDocument/Renderer/Overlay    libass 字幕（ASS/LRC）
│   ├── Decoder/Demuxer/AudioDevice/AudioResampler/AudioTempo
│   └── FFmpegPlayerPlugin.cpp      3 行，qmlRegisterType<VideoPlayer>(uri,1,0,"VideoPlayer")
├── patches/                  FFmpeg 3.4.8 补丁（binutils、rkmpp 新 MPP、openssl3、DASH、TLS）
├── external/alsa-lib-1.1.5/  ALSA 头文件
├── external/rkmpp/           MPP/libdrm 头文件 + pkg-config（标称 MPP 1.3.10 / libdrm 2.4.121）
├── external/dictpen/         从设备拉取的 libasound/libdrm/librockchip_mpp（**与设备 md5 一致**）
└── xmake.lua                 静态 FFmpeg 3.4.8 + OpenSSL 3.5.7 + libxml2 + libass 0.17.4
                              + freetype/harfbuzz/fribidi，产物 qml/FFmpegPlayer/{libffmpegplayerplugin.so,qmldir}
```

- QML 接口：`open(video,audio,headers)`、`play/pause/stop`、`seek/seekForward/seekBackward`、
  `playbackRate`(0.5–2.0)、`boostRate`(长按倍速)、`position/duration`、`subtitleSource/subtitleDelay/subtitleFontFile`、
  `progressBarEnabled/Color/Height`、`videoDecoder/hardwareDecoding`、`errorOccurred/endOfMedia`。
- 运行时开关（环境变量）：`FFPLAYER_HWDEC`、`FFPLAYER_ZEROCOPY`、`FFPLAYER_GL`、
  `FFPLAYER_SUBTITLE_FONT`、`SSL_CERT_FILE/SSL_CERT_DIR`。
- 导出符号用 `src/ffmpegplayer.exports` 局部化，静态依赖不会抢占宿主已加载的 OpenSSL / FFmpeg。

## 2. 设备侧核实（本次实测）

### 2.1 宿主与工具链（适配良好）

| 项 | 实测 | 影响 |
|---|---|---|
| 设备 Qt | `/usr/lib/libQt5Core.so.5.15.2`，与本仓库 `aarch64-linux-qt-5.15.2` 为**同一次 buildroot 构建**（`.prl` 中 build 目录一致，`bin/moc` 软链到宿主 `/usr/bin/moc`） | 插件 ABI 天然匹配，可直接用现有 `--qt=` 配置 |
| 宿主图形栈 | `QQuickView` + OpenGL；`YoudaoDictPen` NEEDED 含 `libmali.so.1`、`libGLESv2.so.2` | 插件 `QSGRendererInterface==OpenGL` 前置条件满足（仍需 POC 实测） |
| libmali | `libmali-bifrost-g31-rxp0-wayland.so`，导出 `EGL_EXT_image_dma_buf_import`、`GL_OES_EGL_image_external` | 硬解可用时（§2.4）零拷贝路径有 GL 侧支持 |
| 注入点 | `src/mod/Engine.cpp` 在 `initUi` 里、`origin(self)`（加载主 QML）之前拿到 `QQuickView*` 与 `rootContext()` | 可在此 `engine()->addImportPath(...)`，外部 QML 模块可被主 QML `import` |
| 环境变量 | 宿主 environ **未设置** `QML2_IMPORT_PATH` | 只能靠 `addImportPath()`（或改 `/usr/bin/runDictPen`） |
| 工具链 | zig 0.16.0、xmake 3.0.4、patch/make/pkg-config 齐全 | 与上游 README 的构建前置一致（上游用 `--toolchain=zigcc`） |

### 2.2 音频

- 插件用 ALSA `default` 设备；设备 `/etc/asound.conf` 把 `default` 插件化（`plug_ply`→`softvol_ply`→`hw:0,0`），可直接出声。
- 插件在打开 ALSA 时创建 `/tmp/audio_wakelocks/ffmpegplayer-<pid>-*.lock`，关闭时删除；
  PenMods `src/system/sound/AudioDaemon` 正是 inotify 监听该目录并统计 `*.lock` 决定是否关闭音频输出。
  **两套机制天然对齐**（该目录在当前设备上已存在且为空）。
- 注意：插件绕过了 `YSoundCenter`，音量/音效（softvol、eq_drc）行为与宿主播放器不完全一致，需实测。

### 2.3 字幕 / HTTPS / 字体

- 默认字幕字体路径 `/usr/lib/fonts/NotoSansSC-Regular.otf` **存在**（该目录共 152 个字体，含 SC/JP/KR）；
  也可用 `subtitleFontFile` 指向 `/userdisk/PenMods/plugins/*/qml/*.ttf`。
- `/etc/ssl/certs` 有 441 个 hash 证书 → HTTPS 校验可用（插件静态链接 OpenSSL 3，与宿主 `libssl.so.1.1` 隔离）。

### 2.4 硬解：出厂固件上不可用，但可以被「patch 内核/设备树」打开

现状（出厂固件）：

| 证据 | 结果 |
|---|---|
| 运行中 DT `/proc/device-tree/vpu_combo/status` | `disabled`（子节点 `vpu_service@ff442000`/`hevc_service@ff440000` = `rockchip,vpu_sub`/`hevc_sub`，自身无 `status`） |
| `/sys/bus/platform/drivers/rk-vcodec`、`mpp`、`mpp_dev` | 驱动都在，但**没有任何设备绑定**；`/proc/kallsyms` 有 `vpu_service_open`、`mpp_srv_*` |
| `/dev/vpu_service`、`/dev/mpp_service`、`/dev/ion`、`/dev/dma_heap` | 全部不存在；`/proc/kallsyms` 里 `ion_` 符号数为 **0**（内核压根没编 ION） |
| `mpv --hwdec=rkmpp`（自带新 MPP 库） | `[vd] Looking at hwdec h264_rkmpp-rkmpp...` → `Could not create device.` → 回落软解（**不崩**） |
| `gst-launch-1.0 ... ! mppvideodec`（厂商 gst 元素，rank 257） | `mpp_rt: found drm allocator` 之后 **Caught SIGSEGV** |
| 自写探针 `mpp_check_support_format()` + `mpp_create()` + `mpp_init()` | 前两步返回 0（OK），`mpp_init()` 在 `hal_h264d_init` 断言后 **段错误（exit 139）** |

**这是硬解路径最大的坑**：厂商 MPP 库的能力预检（`mpp_check_support_format`）只查 SoC 支持表、不查内核设备，
所以 FFmpeg 的 `rkmpp_init_decoder` 会一路走到 `mpp_init()` 然后**崩死整个进程**——
上游插件「`avcodec_open2` 失败就回落软解」的逻辑根本没机会执行。

可打开该节点的证据（已在设备上核实）：

| 对象 | DTB 中 `vpu_combo.status` |
|---|---|
| 现网已安装的 boot 槽（`/dev/mmcblk1p6`/`p7`）与 recovery（`p8`） | `disabled`（三个 DTB 全 disabled，与运行时 DT 一致） |
| 维护者准备的 `/userdisk/boot.img` | husb311 版（本机型号）= `okay` ✅；exam 版 #1 = `okay`、exam 版 #0 = **`okey`（拼写错）** |
| `/userdisk/new_boot.img` | 只有 exam 版 DTB，且是 `okey` → **不会生效** |

> `of_device_is_available()` 只认 `okay`/`ok`，`okey` 等价于 disabled——刷 `new_boot.img` 会得到「什么都没变」的现象。

打开后需要成立的链路（与内核源码一致，已比对 rockchip `release-4.4` 的 `vcodec_service.c`）：

```
DTB: vpu_combo.status = "okay"（+ 子节点 allocator = <1>）
  └─ rk-vcodec 驱动 probe → /dev/vpu_service       ← 4.4 传统路径
       └─ MPP 用户态（系统 /usr/lib/librockchip_mpp.so.0，正是上游 external/dictpen 里那份）
            ├─ 缓冲区：驱动打印 "%s allocator with mmu %s"，allocator==1 → **drm**（不是 ion）
            └─ MPP 运行时还会自己挑分配器：实测 “NOT found ion allocator / found drm allocator”
               → `/dev/dri/card0`，所以即使 FFmpeg 3.4.8 里硬编码 `MPP_BUFFER_TYPE_ION`（rkmppdec.c:226）
                 也能跑（已用探针验证 group/buffer 申请+释放全部 OK）
```

即：**内核侧不需要 ION**，用户态也不需要额外补丁；缺的只是那一个 DTB 属性。
上游 `xmake.lua` 也允许两种情况：`external/dictpen` 里保留 MPP/libdrm 就编出带硬解版本，删掉就纯软解。
无论编哪种，运行时都必须先判 `/dev/vpu_service` 是否存在（见 §5）。

### 2.5 软解性能实测（同一台设备，mpv `--hwdec=no`，4×Cortex-A35@1.2 GHz）

| 素材 | 结果 | 结论 |
|---|---|---|
| 640×360@30 H.264 high | 600 帧 / 2.285 s ≈ **262 fps** | 约 8× 实时 |
| 1280×720@30 H.264 high | 600 帧 / 5.813 s ≈ **103 fps** | 约 3.4× 实时 |

→ 硬解缺失不影响词典笔上的视频播放（720p 及以下软解余量充足），
代价是 CPU 占用与功耗（电池设备需要留意）。

## 3. 与现有播放路径对比

| 路径 | 实现 | 旋转（左右手） | 画面内 UI | 字幕 | 备注 |
|---|---|---|---|---|---|
| `externalPlayer` → `/userdisk/mpv` | 独立 mpv 进程（Wayland 客户端） | 只能靠 `--video-rotate=180` 打补丁 | 不可能（另一个进程/表面） | mpv 自带（ASS 可用） | 已有兜底，格式支持最广，崩溃隔离最好 |
| `qml/audiopages/VideoPlayer.qml` | QtMultimedia `Video` → GStreamer `waylandsink` | ❌ 合成器 overlay 不跟随 `YMainWindow.rotation`，左手模式画面朝向错 | ❌ 控件无法叠加在视频上 | 无 | 现网内置路径 |
| FFmpegPlayer 插件 | 进程内、进 QSG 场景图 | ✅ 自动跟随应用旋转 | ✅ 进度条/倍速/手势/字幕都有现成 QML 示例 | ✅ libass 渲染 ASS，LRC 歌词 | 需静态 FFmpeg；崩溃影响宿主 |

> 现网内置视频页实际上也是硬的：gst 的 `mppvideodec` rank = **257**（高于 FFmpeg 的软解），
> 而它在无 VPU 的固件上会 SIGSEGV（§2.4）——这大概就是 `.mp4` 被改走 mpv 的原因。
> 打开 VPU 后这一页也会跟着能硬解，但它仍是 overlay，旋转/控件/字幕问题不变。

## 4. 集成方案

### 方案 A（推荐）：仓库内新增 target，按可选组件部署

1. `xmake.lua` 增加 `FFmpegPlayerPlugin` target（照抄上游 `xmake.lua` 的包配方；`rkmpp` 建议**保留**，
   因为同一份 .so 要靠运行时预检决定用不用硬解），产物复制到 `build/.../qml/FFmpegPlayer/`。
2. 部署到 `/userdata/PenMods/qml/FFmpegPlayer/`（可选组件，缺失时自动回落旧路径）。
3. `Engine.cpp`：`view.engine()->addImportPath("/userdata/PenMods/qml")`（必须在 `origin(self)` 之前）。
4. QML 侧：`import FFmpegPlayer 1.0`，把 `Video {}` 换成 `FFmpegPlayer.VideoPlayer { source: ... }`，
   控件/手势直接移植 `examples/Player.qml`（该示例就是为 320×170 触摸屏写的）。
5. 与 `mediaSession` 桥接（把播放状态/进度/标题喂给 `src/media/MediaSession`），让快捷设置面板可控。
6. 硬解门控：启动时（`BeforeMain`/`beforeUiInitialization`）`access("/dev/vpu_service", F_OK)`，
   不存在就 `setenv("FFPLAYER_HWDEC", "0", 1)`——否则插件会去调 `mpp_init()` 把宿主崩掉（§2.4）。
7. 开关：参考 `src/tweaker/OcrBackend.cpp` 的「文件式开关」做法（存在即启用），失败/缺失即回落 mpv。

### 方案 B：做成 PenMods 插件包

`/userdisk/PenMods/plugins/ffmpegplayer/{metadata.json, libffmpegplayerplugin.so, qml/FFmpegPlayer/...}`，
插件页 `main.qml` 用相对目录 `import "FFmpegPlayer"`（目录内 `qmldir` 指向插件 so）。
优点是核心仓库保持干净、可单独分发；缺点是**只能在自己的插件页面里用**，无法替换内置视频页。

### 方案 C：最小侵入

只替换 `audiopages/VideoPlayer.qml` 里的 `Video {}`，`FileManagerPageComponent.qml` 的两处
`externalPlayer.open()` 保持不变（`.mp4` 仍走 mpv）。适合先验证再决策。

## 5. 风险清单

| 风险 | 说明 | 缓解 |
|---|---|---|
| 进程内崩溃放大 | 解码/GL 崩溃会拖垮 `YoudaoDictPen`；连续崩溃会被固件写成 `boot-recovery`（见 AGENTS.md 的 recovery 流程） | 开关 + 自动回落 mpv；灰度；先只在新页面试点 |
| 内存 | 整机 460 MB，`top` 显示 free 仅几 MB（靠 cache/swap）；静态 FFmpeg 的解码队列/缓冲会长期占用 | 实测对比「内嵌 vs mpv 独立进程」总内存；限制队列/分辨率 |
| 构建与 CI | 首次 `xmake require` 需联网编译 OpenSSL3 + libxml2 + FFmpeg 3.4.8 + libass + freetype/harfbuzz/fribidi，本机耗时以十分钟计；现有 CI 只构建 `libPenMods.so` | 决定「CI 加长构建」还是「本地构建 + 随 OTA 分发预编译 so」 |
| 体积 | 产物预计 10–20 MB（静态 FFmpeg/libass/OpenSSL） | 部署到 `/userdisk`（3.4 GB 余量），不要放 `/userdata`（仅 164 MB 余量） |
| GL 路径差异 | 零拷贝/External 纹理在软解下不会启用，但插件仍会在宿主 GL 上下文里编译 shader | POC 实测（见 6） |
| 音频语义 | 直连 ALSA，绕过 `YSoundCenter`；音量键/音效是否生效需实测；异常退出可能残留 `.lock` | 实测；`AudioDaemon` 侧已按 PID 判断锁有效性 |
| 宿主符号冲突 | 宿主已加载 OpenSSL 1.1、GStreamer/Mpp 插件 | 上游已用 version script 局部化静态符号，需复核导出表 |
| **无 VPU 时 MPP 必崩** | 出厂固件上 `mpp_init()` 段错误（实测：自写探针、厂商 `mppvideodec`、ffmpeg rkmppdec 都会走到这一步），会直接带走宿主进程 | 硬解尝试前判 `/dev/vpu_service`（不存在则 `setenv("FFPLAYER_HWDEC", "0")` 或先试子进程）；插件侧 `FindHwDecoder()` 只看分辨率/像素格式，没有设备预检，必须由 PenMods 兜 |
| 内核补丁的分发 | PenMods 是 LD_PRELOAD 用户态 mod，改不了 DTB；`boot.img` 只能作为独立的固件步骤 | 开关 + 文档说明；未打补丁的设备自动走软解 |

## 6. 建议的最小验证步骤（POC）

0. **先验 VPU（内核侧，独立于本插件）**：刷入 `/userdisk/boot.img` 后确认
   `cat /proc/device-tree/vpu_combo/status` = `okay`、`ls -l /dev/vpu_service` 存在；
   用 `tools/mpp-probe` 确认 `mpp_init(dec, h264)` 返回 0（不再段错误），再试 `mpv --hwdec=rkmpp`。
   （`new_boot.img` 的 `okey` 拼写不生效，别用它判断。）
1. **软解构建**：想先跑通软解就删掉 `external/dictpen/librockchip_mpp.so.1`、`libdrm.so.2`，
   否则保留（正式方案应保留 + 运行时门控）：
   `xmake f -c -p linux -a arm64-v8a -m release --toolchain=zigcc --cross=aarch64-linux-gnu.2.27 --qt=$HOME/PenMods/aarch64-linux-qt-5.15.2`
   → `xmake require -v ffmpeg && xmake require -v libass` → `xmake -v ffmpegplayerplugin`；
   记录耗时与最终 `.so` 体积。
2. **设备侧冷启动验证**：把 `qml/FFmpegPlayer` 推到设备，用上游的独立 demo（`examples/`）验证
   渲染、音频、字幕、（缺失时）回落行为——先不碰宿主进程。
   打开 VPU 后额外看 `videoDecoder` / `hardwareDecoding` 是否真的变成 `*_rkmpp`/true。
3. **宿主内验证**：`addImportPath` + 一个新页面（方案 C），用截图/`applog` 确认
   画面方向、叠加、旋转、崩溃与内存表现（无 VPU 时确认 `FFPLAYER_HWDEC=0` 生效、不产生 SIGSEGV）。
4. 通过后再考虑替换内置视频页 + `mediaSession` 桥接 + OTA 分发。

> 崩溃排查可借助 `ulimit -c` + `/userdisk/corefile/`（`runDictPen` 已配 `core_pattern`），
> core 里 `librockchip_mpp.so` 的栈顶就能确认是不是 `mpp_init` 那条路。

## 7. 参考

- 上游 README / `xmake.lua` / `examples/Player.qml`（本仓库未内置，分析时取自 v1.0.0）
- 本仓库：`tools/mpp-probe/`（本文 §2.4 的可复现探针与实测输出）、
  `src/mod/Engine.cpp`（QML 注入点）、`src/filemanager/player/ExternalPlayer.cpp`（mpv 路径）、
  `resource/models/YDP02X/qml/audiopages/VideoPlayer.qml`（现有 QtMultimedia 页）、
  `src/system/sound/AudioDaemon.h`（`/tmp/audio_wakelocks` 约定）、`src/media/MediaSession`
- 内核侧参照：rockchip `release-4.4` 的 `drivers/video/rockchip/vcodec/vcodec_service.c`
  （`allocator` 属性 1=drm / 2=ion 的映射就写在这个文件的 `dev_info()` 里）
- AGENTS.md：设备部署/恢复流程、`Commands That Must Run Outside the Sandbox`

---

## 8. POC 实测结果（2026-10-07，YDP02X 2D90500000818624）

结论：**在词典笔上嵌入成功**，视频在应用进程内、跟随 UI 旋转、字幕/音频/唤醒锁都正常。

### 8.1 构建（上游 v1.0.0，本机交叉编译）

| 项 | 结果 |
|---|---|
| 命令 | `xmake f -c -p linux -a arm64-v8a -m release --toolchain=zig --cross=aarch64-linux-gnu.2.27 --qt=$HOME/PenMods/aarch64-linux-qt-5.15.2` + `xmake require ffmpeg/libass` + `xmake ffmpegplayerplugin` |
| 产物 | `libffmpegplayerplugin.so` **15.6 MB**（Qt 动态、静态 libc++、FFmpeg 3.4.8 + libass + OpenSSL3 + libxml2 全静态） |
| NEEDED | `libasound.so.2` + `libQt5Quick/Qml/Gui/Core` + libc（无 libstdc++/无 librockchip_mpp，见下） |
| 耗时 | 首次约 40 min（需要从源码编译 python/harfbuzz/libass/freetype/fribidi/openssl3/libxml2/ffmpeg） |

构建过程踩到的三个坑（本机环境问题，非上游 bug）：

1. `add_deps("openssl3 3.5.7")`：本地 xmake-repo 没有该版本（最新 3.6.x / 有 3.5.6），临时降到 `3.5.6`。
2. xmake-repo 的 `python` 包默认 `--enable-optimizations`（PGO），在本机会因为 CPython 自带用例
   `test_generators.SignalAndYieldFromTest` 偶发失败而中断；本机已把该参数去掉
   （`~/.xmake/repositories/xmake-repo/packages/p/python/xmake.lua`，可 `git checkout` 还原）。
   python 是通过 `harfbuzz → meson` 链进来的。
3. `external/dictpen/*.so` 在 git 里是符号链接，用 zip 下载后变成 14–20 字节的文本文件，
   会导致 `-lrockchip_mpp/-lasound/-ldrm` 链接失败；解包后要补成真正的符号链接。

另外注意：本次构建的 FFmpeg **没有编入 rkmpp**（日志：`rkmpp disabled (needs linux/arm64 and
external/dictpen/librockchip_mpp.so.1 + libdrm.so.2)`，虽然两个文件都在）。
结果是插件 `FindHwDecoder()` 找不到 `h264_rkmpp` → 直接走软解，反而天然规避了 §2.4 的崩溃。
想要硬解版本需要确认这个 `has_device_libs()`/`is_arch` 判定为什么没生效（可先临时把
`configs.rkmpp` 写死为 `true` 验证），再重编 ffmpeg 包。

### 8.2 设备侧（三步验证）

| 步骤 | 结果 |
|---|---|
| ① 独立进程（`qmlscene`，不碰宿主） | `FFmpeg Core Initialized (Lavf57.83.100)` → `Using software decoder: h264` → 20 s 素材播完 `endOfMedia`，position 实时推进，无错误 |
| ② 宿主内嵌（PenMods QML 页 `import FFmpegPlayer`） | `Video renderer: OpenGL YUV` —— **QSG 节点在宿主 GL 上下文里初始化成功**，截图可见视频画面填满页面 |
| ③ 左右手模式 | 切成左手（`righthandmode=false`）后视频**随 UI 一起 180° 旋转**（截图确认），mpv 时代做不到 |
| 音频 | `/tmp/audio_wakelocks/ffmpegplayer-<pid>-*.lock` 出现，`AudioDaemon` 记录 `fileLocks=2 → 强制 openAudioOutput` ✓；退出页面后锁文件消失 |
| 返回/停止 | 标题栏返回键 `stop()` → 锁文件立即释放，应用存活 |
| 资源占用 | 空闲 RSS 182 MB(49 线程) → 播放 640×360 H.264+AAC 时 RSS 223 MB，进程 CPU ≈30%（单核占比） |

### 8.3 为了让"嵌入"跑起来所做的 PenMods 改动（最小集）

| 文件 | 改动 |
|---|---|
| `src/mod/Engine.cpp` | `initUi` 里 `origin(self)` 之前：目录存在则 `view.engine()->addImportPath("/userdata/PenMods/qml")` |
| `src/mod/Mod.cpp` | `BeforeMain`：无 `/dev/vpu_service` 且未显式设置时 `qputenv("FFPLAYER_HWDEC","0")`（防 §2.4 崩溃） |
| `src/filemanager/player/ExternalPlayer.cpp` | 检测到插件存在时 `open()` 只记文件名、不再 `startDetached` mpv（保留 mpv 作为兜底） |
| `resource/models/YDP02X/qml/audiopages/ExternalPlayer.qml` | 原本是空壳页，改成 `FFmpegPlayer.VideoPlayer { source: externalPlayer.path; autoPlay: true }` + 标题栏返回键 |
| 部署 | 插件模块 `/userdata/PenMods/qml/FFmpegPlayer/{libffmpegplayerplugin.so,qmldir}`；`libPenMods.so` / `libPenModsResources.so` 按 tmp+rename 流程更新 |

### 8.4 本次测试用到的调试手段（可复用）

- **Weston 截图**：本机出厂 Weston **没有** `--debug`，`weston-screenshooter` 会报
  `permission denied. Debug protocol must be enabled`。备份 `/etc/init.d/S50launcher`
  （本次留存在 `/userdisk/S50launcher.penmods-bak`），在 weston 命令行加 `--debug` 后重启生效。
  截图是 170×320 竖屏原始帧缓冲，需 `rotate(270, expand=True)` 才是逻辑 UI。
- **触摸注入**：AGENTS.md 里描述的 uinput 绝对指针方案可用，本次实现为
  `/tmp/ffp/injector.c`（`tap/longtap/swipe` + `--lefthand`），导航路径
  `首页 → 听力练习(第 5 项，需横滑) → 文件管理 → bili → *.mp4`。
- `qmlscene` 在设备上存在（`/usr/bin/qmlscene`），配合 `QML2_IMPORT_PATH` 可以**不碰宿主**先验证插件；
  注意该环境下 `Item.grabToImage()` 回调不触发（拿不到离屏抓帧），所以最终验证仍靠 compositor 截图。

### 8.5 硬解版本（rkmpp）跟进结果

**为什么第一版没有 rkmpp**：同一份 `xmake.lua` 在不同时间点解析出了不同结果 ——
`~/.xmake/packages/f/ffmpeg/3.4.8/` 下同时存在
`2d83778…`（rkmpp=true, revision 7）与 `18ee3907…`（rkmpp=false, revision 7）两个变体，
而 `xmake require -y -v ffmpeg` 会把解析拉回 rkmpp=false 的那一个（即使项目里写的是 true），
随后 `xmake build` 就链接了软解版。稳妥做法是**把配置钉死并让旧构建失效**：

```lua
-- 项目 xmake.lua，add_requires("ffmpeg 3.4.8", {...}) 里
network_revision = 8,   -- 换一个值即可强制重编（recipe 自带该开关）
rkmpp = true,           -- 不要再用 has_device_libs() 的返回值
```

钉死后重编得到的变体 `1402af5e…`：`libavcodec.a` 里有 16 个 rkmpp 符号，插件
`NEEDED` 出现 `librockchip_mpp.so.1` / `libdrm.so.2`，`nm -D -u` 有 30 个 `mpp_*` 未定义符号，
`.so` 12.67 MB。

**崩溃预测被实测证实**（独立进程 `qmlscene`，不设 `FFPLAYER_HWDEC`，出厂固件）：

```
mpi: mpp version: Without VCS info
hal_h264d_api: Assertion vcodec_type & ((0x00000200)|(0x00000001)|(0x00000002)) failed at hal_h264d_init:104
hal_h264d_api: hal_h264d_init hard mode error, value=0
mpp_device: mpp_device_init failed to find device for coding 7 type 0
mpp_rt: NOT found ion allocator / found drm allocator
[1145.685336] qmlscene[6540]: unhandled level 2 translation fault (11) at 0x00000000, esr 0x82000006
[1145.689219] PC is at 0x0            ← librockchip_mpp 里回落到空指针调用
```

**门控被实测证实有效**（宿主内，同一个 rkmpp 版本插件）：`Mod.cpp` 的
`/dev/vpu_service` 探测让插件拿到 `FFPLAYER_HWDEC=0`，
应用日志出现 `Using software decoder: h264/aac` + `Video renderer: OpenGL YUV`，
进程存活、截图正常、唤醒锁正常释放。

**设备环境的坑**：本机 `adbd` 的环境里带着 `FFPLAYER_HWDEC=0`
（`cat /proc/$(pidof adbd)/environ`），所以从 `adb shell` 启动的任何 Qt 进程都会静默走软解；
宿主应用不受影响（它由 guardian 启动，日志里能看到门控警告）。

**还没做到的一步**：`hardwareDecoding=true` 只在 VPU 真正可用时才会出现，
也就是要刷入 §2.4 里那版 `vpu_combo.status = "okay"` 的 `boot.img`。刷完后应看到：
`/dev/vpu_service` 出现 → `Mod.cpp` 的警告消失 → 插件日志变成
`Using hardware decoder: h264_rkmpp` 且 `hardwareDecoding` 为 true。

---

## 9. 仓库集成（已落地）

```
external/ffmpeg-player/          上游 v1.0.0（48ca849）的 vendor 副本，GPL-3.0
├── xmake.lua                    已适配为 PenMods 根工程的子工程（见 UPSTREAM.md 的差异表）
├── UPSTREAM.md                  来源、改动、同步方式、硬解门控说明
├── src/ patches/ examples/      上游原样
└── external/{alsa-lib-1.1.5,rkmpp,dictpen}   上游原样（dictpen 里的 .so 在 git 中是符号链接）
```

根工程改动：

| 位置 | 内容 |
|---|---|
| `xmake.lua` | 文件尾 `includes('external/ffmpeg-player')`（硬解/软解由子工程按设备库是否存在判断，`PENMODS_FFMPEG_PLAYER_SW=1` 可强制软解） |
| `.gitignore` | 无需改（`build/*`、`.xmake` 已在忽略列表；子目录自带 `.gitignore`） |
| `scripts/deploy_ffmpeg_player.sh` | 部署 `qml/FFmpegPlayer/` 到 `/userdata/PenMods/qml/`，`.so` 走 tmp+rename，最后重启宿主 |
| `src/mod/Engine.cpp` | `addImportPath("/userdata/PenMods/qml")`（主 QML 加载前） |
| `src/mod/Mod.cpp` | 无 `/dev/vpu_service` 时 `FFPLAYER_HWDEC=0` |
| `src/filemanager/player/ExternalPlayer.cpp` | 插件存在时不再拉起 mpv |
| `qml/audiopages/ExternalPlayer.qml` → `resource/models/YDP02X/qrc_qml.h` | 空壳页改成 `FFmpegPlayer.VideoPlayer`，用 `scripts/gen_qt_res.sh` 重新生成资源头 |

构建与部署：

```sh
xmake f --qt="$HOME/PenMods/aarch64-linux-qt-5.15.2" --arch=arm64-v8a --build-platform=YDP02X \
  --target-channel=dev --toolchain=zig -m release -vD --cross=aarch64-linux-gnu.2.27 -c
PENMODS_WITH_PLAYER=1 xmake build ffmpegplayerplugin   # 插件默认不参与工程解析，需显式开启
                                                      # 产物 build/linux/arm64-v8a/release/qml/FFmpegPlayer/
./scripts/deploy_ffmpeg_player.sh
```

实测（用仓库内构建的产物部署到设备后）：`Using software decoder: h264/aac` +
`Video renderer: OpenGL YUV`，应用存活、截图正常、唤醒锁正常释放；
产物 12.55 MB（硬解版），`NEEDED` 含 `librockchip_mpp.so.1`/`libdrm.so.2`。

构建期的两个坑（已写进 `UPSTREAM.md`）：

1. **不要在 PenMods 工程里跑 `xmake require ffmpeg`** —— 它按 recipe 默认值
   （`network_revision=7`、`rkmpp=false`）再装一个软解变体，紧接着的 `xmake build`
   会链接那个变体，表现为"配置开了硬解但产物没有 MPP 依赖"。`xmake f`/`xmake build`
   本身会按工程配置装依赖。
2. `qt.moc` 生成的 moc 单元在 `-r` 重建时可能丢掉 `add_packages` 的 include 目录，
   子工程里已在 `on_load` 显式补上 ffmpeg/libass 的 include。

### 8.6 硬解实测（已刷入 VPU 补丁的 boot）

**刷机记录**（YDP02X / husb311，2026-10-07）

```sh
# 1. 备份当前 boot_a（= /dev/mmcblk1p6，slot_suffix=_a）
dd if=/dev/block/by-name/boot_a of=/userdisk/boot_a.stock-backup.img bs=1M
#    md5 = 16304b29885cc8a761e96b8fdf5765e4（与运行中的镜像一致）
# 2. 写入维护者准备的补丁镜像并回读校验
dd if=/userdisk/boot.img of=/dev/block/by-name/boot_a bs=1M conv=fsync
dd if=/dev/block/by-name/boot_a of=/tmp/verify.img bs=1M
md5sum /tmp/verify.img /userdisk/boot.img     # 两边都是 3af8252994ba3d7fa98f52fb5c3650a3
```

重启后（一次成功，无 recovery 往返）：

| 检查 | 结果 |
|---|---|
| `/proc/device-tree/vpu_combo/status` | `okay` |
| `/dev/vpu_service`、`/dev/hevc_service` | 都存在（245/246） |
| `dmesg` | `rk-vcodec vpu_combo: init success`、`platform ff442000.vpu_service: drm allocator with mmu enabled`（两个 sub 设备都是 drm，与 §2.4 推断一致） |
| `/dev/mpp_service`、`/dev/ion`、`/dev/dma_heap` | 仍然不存在（不需要） |

**MPP 探针**（`tools/mpp-probe`）：`mpp_init(dec,h264) ret=0 OK`，不再段错误；ION/DRM 两组
buffer 申请/释放照旧成功 → 之前那条"没有 /dev/ion 怎么办"的疑问彻底排除。

**插件（独立进程 qmlscene）**

```
【 I 】Decoder.hpp:61        Using hardware decoder: h264_rkmpp
【 I 】FFmpegCore.hpp:35     [FFmpeg] [h264_rkmpp] Decoder noticed an info change (640x360), format=0
【 I 】VideoRenderNode.hpp:778  Video renderer: OpenGL YUV
【 I 】VideoRenderNode.hpp:355  dma-buf zero-copy rendering enabled
qml: [test] pos=5.8 hw=true dec=h264_rkmpp          ← hardwareDecoding 属性实测为 true
qml: [test] endOfMedia                              ← 20s 素材完整跑到 EOF，无崩溃
```

**插件（宿主内）**：同样的 4 行日志（`h264_rkmpp` + `dma-buf zero-copy rendering enabled`），
应用存活、截图正常、唤醒锁正常创建/释放；因为 `/dev/vpu_service` 存在，
`src/mod/Mod.cpp` 的门控这次没有介入（应用环境里没有 `FFPLAYER_HWDEC`）。
注意 `Mod.cpp` 的门控要保留：换成没打补丁的固件时它会重新变成"强制软解"。

**CPU 对比**（同一 640×360 H.264 素材，独立 qmlscene，5 s 采样 `/proc/<pid>/stat`）

| | CPU 占用 |
|---|---|
| 软解（`hw=false, h264`） | **78%** |
| 硬解（`hw=true, h264_rkmpp`） | **37%** |

**遗留观察**

- 解码时内核会周期性打印 `rk_vcodec: vpu_service_ioctl:2138: error: unknown vpu service ioctl cmd 40086c01`
  （一次会话十几次）。功能不受影响（软硬件解码、渲染、seek 都正常），像是 MPP 用户态在探测
  这版 4.4 内核驱动没实现的新 ioctl，暂不处理。
- `/userdisk/boot.img` 里 exam 版的两个 DTB 有一个写成 `status = "okey"`（本机 husb311 那份是 `okay`，
  所以不影响这台机器）；如果以后要用这个镜像刷 exam 版机器，记得先改回 `okay`。
- 备份留在设备上：`/userdisk/boot_a.stock-backup.img`（出厂 boot_a），`/userdisk/boot.img`（补丁镜像）。

---

## 10. 界面控制与「内核补丁开关」（2026-10-07 晚）

### 10.1 播放页控件（`qml/audiopages/ExternalPlayer.qml` 重写 + `YVideoOsdButton.qml`）

| 交互 | 行为 |
|---|---|
| 单击画面（OSD 隐藏时） | 只唤出 OSD（含左上返回按钮），不改变播放状态 |
| 单击画面（OSD 显示时） | 播放/暂停 |
| OSD 显隐 | 显示后 3 s 自动隐藏（播放中才隐藏）；**左上角返回按钮和右下角提示一样，属于 OSD，隐藏时整条不可见也不接收点击**，画面全程干净 |
| 长按 400 ms | 临时 `boostRate`（默认 2.0x），右下角显示「长按 2.0x」徽标，松手恢复 |
| 横滑 | 快进/快退（整屏宽度 = 60 s），拖动时顶部显示目标时间、且用 `progressBarPosition` 只做预览，松手才 `seek()`；暂停状态保持不变 |
| OSD 按钮 | `暂停/播放`、`-10s`、`+10s`、倍速（0.5/1.0/1.5/2.0 循环，非 1.0 时高亮）、`字幕` 开关 |
| 左下胶囊 | 文件名（超出省略）+ `当前 / 总时长` |
| 底部进度条 | 由插件自绘（`progressBarEnabled`，2 px，青色），不占触摸区域 |
| 字幕 | 打开视频时自动找同目录同名 `.ass` / `.lrc`（`externalPlayer.subtitlePath`），有则默认开启；字体固定 `/usr/lib/fonts/NotoSansSC-Regular.otf` |
| 返回 | `stop()` 后退出页面（唤醒锁随之释放） |

设备实测（640×360 testsrc2 素材 + 同名字幕，硬件解码）：播放/暂停、横滑跳到 00:20、
长按出现「长按 2.0x」徽标、ASS 字幕（顶部/底部）按 libass 渲染、进度条与
`clip 00:04 / 00:20` 胶囊都正常，屏幕截图逐一确认；QML 侧无警告
（`title`/`subtitlePath` 补了 `NOTIFY`，不然每次 position 变化都会刷
`depends on non-NOTIFYable properties`）。

### 10.2 `vpuUnlock`：把 VPU 补丁做成实验性开关

`src/tweaker/VpuUnlock.{h,cpp}`（QML 上下文属性 `vpuUnlock`，
开关在 **更多设置 → 系统微调 → 实验性功能**，紧跟触摸校准）：

- `enabled` = 运行中设备树里 `vpu_combo/status` 是否为 `okay`/`ok`；
- `supported` = root + `vpu_combo` 节点存在 + 当前 slot 的 boot 分区可写；
- 打开：把当前 boot 分区备份成 `/userdisk/PenMods/boot_stock.img`（只做一次），
  复制一份并在**镜像内就地改写 DTB**，存为 `/userdisk/PenMods/boot_vpu.img`，
  再 `dd` 回 boot 分区并**回读比对 md5**；任何一步失败立刻把出厂镜像刷回去，然后重启整机；
- 关闭：把 `boot_stock.img` 刷回去再重启（没有备份则拒绝并提示）。

**FDT 改写的两个要点**（都在实测中踩到过）：

1. 只改**属性值**、不动 `length` 字段：`disabled\0`（9 字节，补齐到 12）改成
   `okay\0\0\0\0` 时如果把 length 也改成 5，后续 token 的起始位置会前移 4 字节、
   整个 blob 立刻解析失败（`bad token`）。保留 length=9 后字符串仍以第一个 NUL 结束，
   `of_device_is_available()` 的 `strcmp(status, "okay")` 一样成立，镜像结构完全不变
   （出厂镜像 vs 补丁镜像只差 24 字节）。
2. 匹配节点名时不能假设深度（`vpu_combo` 在根节点之下而不是第一层），
   否则会一个节点都改不到 —— 这个 bug 被"用 Python 独立实现算一遍再对比"抓出来了。

**round-trip 实测**（开关驱动，全程无手工 dd）：

| 步骤 | 结果 |
|---|---|
| 关（恢复出厂） | 重启后 `vpu_combo/status` = `disabled`、`/dev/vpu_service` 消失、宿主日志回到 `VPU is unavailable, forcing FFmpeg player software decoding` |
| 开（打补丁） | 重启后 status = `okay`、`/dev/vpu_service` 出现、宿主不再有门控警告、播放器 `Using hardware decoder: h264_rkmpp` + `dma-buf zero-copy rendering enabled` |
| 产物一致性 | 设备上现生成的 `boot_vpu.img` md5 = `80f8f889cfcf839e9dd5652066589946`，与另一份独立的 Python 实现逐字节一致 |

出厂备份留在 `/userdisk/PenMods/boot_stock.img`（与 `/userdisk/boot_a.stock-backup.img` 相同，
md5 `16304b29885cc8a761e96b8fdf5765e4`）；`/userdisk/boot_a.stock-backup.img`
是 §8.6 手工刷机时留下的同一份出厂镜像。

---

## 11. 关闭播放器后的内存回收（实测）

工具：`tools/mem-profile/mem-profile.sh`（RSS / 线程 / dma-buf / 插件映射 / 堆段数）。

一次干净的开关（重启宿主 → 进文件管理器 → 打开视频 → 播放 → 返回）：

| 状态 | RSS | 线程 | dma-buf | 插件映射 |
|---|---|---|---|---|
| 刚启动 | 178.5 MB | 44 | 7 | 0 |
| 进到文件管理器（未开播放器） | 195.3 MB | 44 | 7 | 0 |
| 播放中（硬解 640×360 + 零拷贝） | 228.2 MB | 50 | **26** | 12.45 MB |
| 关闭后 / 再等 10 s | 213.4 / 213.5 MB | **44** | **7** | 12.45 MB |

**正常释放的部分**：线程 50→44（与基线一致，解码/音频/渲染线程都 join 了）、
dma-buf 26→7（多出来的 19 个正是 MPP 硬解帧池，属 DRM/CMA 内存，没有泄漏）、
ALSA 关闭、`/tmp/audio_wakelocks/` 锁文件删除、播放期间多占的 4~6 MB 匿名页回收。

**回收不掉的部分**：

- 一次性约 18 MB：`libffmpegplayerplugin.so` 的 12.45 MB 映射（QML 插件由引擎 dlopen，
  机制上不会卸载）+ 静态 FFmpeg/libass/OpenSSL 初始化、GL program、QML 类型与页面对象。
  其中文件管理器页本身占 ~17 MB，与播放器无关。
- 每次开关的分配器/QML GC 滞留，逐次衰减：

```
cycle 1: +16300 kB (含上面的一次性开销)   cycle 6: +1600 kB
cycle 2:  +2600 kB                        cycle 7:  +140 kB
cycle 3:  +1600 kB                        cycle 8:  +160 kB
cycle 4:  +2400 kB                        cycle 9:  +220 kB   <- 收敛
cycle 5:  +1000 kB
```

即：单次播放峰值 +4~6 MB、关闭后回落；反复开关约 6 次后进入平台期（约 221.7 MB），
`rw_seg`/`anon_rw_seg` 也不随次数增长 —— **有界的保留，不是泄漏**，无需处理。
若以后要更保险，可以照 `OcrBackend` 的做法在播放 N 次后延迟重启宿主，但按实测没有必要。
