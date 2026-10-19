# external/ffmpeg-player —— 上游代码与本仓库的改动

本目录是 [`Haikure/ffmpeg-player`](https://github.com/Haikure/ffmpeg-player) 的 **vendor 副本**
（面向 RK3326 / AArch64 / Qt 5.15 的 QML 视频播放器插件），用来给 PenMods 提供
「画在 Qt 场景图里」的视频播放能力（跟随左右手旋转、画面内控件、ASS/LRC 字幕）。

- 上游版本: **v1.0.3**（commit `d7733e3`，tag `v1.0.3`），缩放时保留双线性过滤并关闭多点降采样。
- 同步状态: `src/` 与 `examples/` 已与上游 **v1.0.3** 同步，保留 PenMods 构建适配与设备库。
- 许可: **GPL-3.0**（与 PenMods 的 GPL-3.0-only 兼容）
- 可行性分析、设备实测与坑位记录: [`doc/FFMPEG_PLAYER_ANALYSIS.md`](../../doc/FFMPEG_PLAYER_ANALYSIS.md)

```
external/ffmpeg-player/
├── xmake.lua          上游工程文件（已做少量适配，见下）
├── patches/           上游对 FFmpeg 3.4.8 的补丁（构建时按文件名顺序应用）
├── src/               插件实现（header-only）+ src/qml/qmldir
├── examples/          上游的宿主接入示例（Player.qml 等，可作 PenMods 播放页参考）
└── external/
    ├── alsa-lib-1.1.5/include/   ALSA 头文件
    ├── rkmpp/                    Rockchip MPP / libdrm 头文件 + 本地 pkg-config
    └── dictpen/                  从设备拉取的 libasound / libdrm / librockchip_mpp
                                  （**git 里是符号链接**，zip 下载会退化成文本文件）
```

## 上游 v1.0.1 相对 v1.0.0 的改动

同步自上游 4 个 commit（`48ca849..62905fb`）：

| 上游 commit | 内容 | 对 PenMods 的影响 |
|---|---|---|
| `6ce356c` fix(audio): preserve playback continuity during rate changes | 变速不再 `seek()` 重定位。新增 `src/AudioTimeline.hpp`（按倍速分段的待播 PCM 时间线）；`Clock` 由 `pts_drift` 改为 `last_pts + last_updated`，新增 `SetAudioClock()`；`AudioTempo::SetRate()` 用 `avfilter_process_command("tempo")` 在原滤镜实例上换速；音频线程在 PCM 输出边界切倍速，纯视频才立即切；`AudioDevice` 用 `fed_frames_`/`released_frames_` 精确报告延迟，`Read()` 带回 generation | **QML API 未变**（`playbackRate`/`boostRate`/`beginBoost()`/`endBoost()` 都在），`ExternalPlayerEmbedded.qml` 无需改。行为变化：切倍速不再跳转、`endBoost()` 总是回到 1.0x、变速不变调 |
| `2c72e00` build(deps): preserve OpenSSL cross-compilation flags | 自带一份 `ffplayer-openssl3` 包（`set_base("openssl3")` + 自定义 `on_install`），把 `buildenvs` 的 `CFLAGS`/`ASFLAGS` 显式传给 make，不再依赖 xmake-repo 的 openssl3 配方 | PenMods 只把版本钉在本地仓库有的 `3.5.6`；包名变化会新建一份 OpenSSL 安装 |
| `4c31f0f` fix(build): refresh FFmpeg dependency compatibility | ffmpeg 包的依赖由 `openssl3` 改名到 `ffplayer-openssl3`，并加 `set_policy("package.librarydeps.strict_compatibility", true)`（依赖名/配置变化后不复用旧安装记录） | ffmpeg 包哈希变化 → **旧缓存变体不再复用，需重编一次 FFmpeg** |
| `62905fb` fix(example): improve player controls | 仅 `examples/Player.qml`：缩放/关闭按钮加按下底色、去掉进度条 `bottomMargin` | 示例文件，宿主未引用，无影响 |

## 上游 v1.0.2 相对 v1.0.1 的改动

完整变更：[v1.0.1...v1.0.2](https://github.com/Haikure/ffmpeg-player/compare/v1.0.1...v1.0.2)。此版本包含：

| 上游 commit | 内容 | 对 PenMods 的影响 |
|---|---|---|
| `06c6e8f` feat(video): add adaptive downsampling and pin OpenSSL 3.5.6 | 按源视频尺寸、显示尺寸和设备像素比进行自适应降采样，并将 FFmpeg 的静态 OpenSSL 依赖固定为 `ffplayer-openssl3 3.5.6` | 改善高分辨率视频缩小后的画质；双方依赖版本一致 |
| `8ee79cc` fix(video): handle sampling mode switches | 新增 `VideoPlayer.downsamplingEnabled`，修复缩放和暂停状态下切换采样模式时的渲染问题 | `ExternalPlayerEmbedded.qml` 可在缩放时关闭降采样与平滑过滤 |
| `0542f84` ci: add FFmpegPlayer release workflow | 增加上游发布构建工作流 | 不影响 PenMods 集成 |
| `4986bed` fix(ci): build host qmake for Qt SDK | 修复上游 Qt SDK 环境下的 qmake 构建 | 不影响 PenMods 集成 |

## 上游 v1.0.3 相对 v1.0.2 的改动

完整变更：[v1.0.2...v1.0.3](https://github.com/Haikure/ffmpeg-player/compare/v1.0.2...v1.0.3)。

`d7733e3` 将纹理过滤与多点降采样解耦：关闭 `downsamplingEnabled` 后，GL 与 CPU 回退仍按 `smooth` 选择线性或最近邻过滤。
示例改为 `downsamplingEnabled: !zoomMode`、`smooth: true`，缩放时以原始纹理做双线性采样；普通模式保持原有降采样行为。
PenMods 的 `qrc_qml.h` 同步纳入对应页面适配。此版本未更改构建配方、依赖、`patches/` 或设备库。

## 与上游的差异

以下差异以上游 **v1.0.3** 的独立工程为参照。
`xmake.lua` 仍保留 PenMods 的集成适配，与上游独立工程的版本不同：

| 改动 | 原因 |
|---|---|
| 去掉文件级 `add_rules("mode.debug","mode.release")`；把 `set_languages("c11","c++17")`、`set_optimize("fastest")`、`set_warnings("all","error")` 移进 target 作用域 | 本文件现在由根工程的 `includes()` 载入，文件级设置会污染 PenMods 自己的 target（它是 c++23、且不能把警告当错误） |
| target 内加 `set_pcxxheader()` | 根工程给所有 target 预设了 PCH `src/base/Base.h`，插件不该用它 |
| 所有 `os.scriptdir()` / 相对路径 → `path.join(os.projectdir(), "external", "ffmpeg-player", …)` | 被 `includes()` 载入后，相对路径的基准是根工程目录 |
| `network_revision = 7` → `8` | 该字段参与 ffmpeg 包哈希，改它可强制重编；也用于让本地旧变体失效 |
| 文件开头加"默认跳过"开关：没有 `PENMODS_WITH_PLAYER=1` 就 `return`，连 `add_requires` 都不声明 | CI 与只编 libPenMods.so 的人不必为一个 40 分钟的依赖链（FFmpeg/OpenSSL3/libass/harfbuzz→meson→python）和容器里的 `unzip`/`perl` 买单；`xmake build` 仍会成功（插件被跳过并打印一行提示） |
| `rkmpp = has_device_libs(...)` → `rkmpp = use_rkmpp`（设备库存在即开启；`PENMODS_FFMPEG_PLAYER_SW=1` 可强制软解） | 保留原语义、去掉隐式行为；不用 xmake option 是因为本机 xmake 在工程加载阶段 `get_config()` 读不到 CLI 传进来的值（实测为 nil） |
| moc 实现头依赖与生成文件路径使用 `external/ffmpeg-player/src/` | 修正 vendor 工程的路径基准，使实现头变化时两个 moc 对象的失效规则指向实际文件 |
| target 加载时显式补充 FFmpeg/libass 的 include 目录 | 确保重建的 moc 编译单元能找到依赖头文件 |

双方现在都使用 `ffplayer-openssl3 3.5.6`，相对原发布基线的版本调整已随 v1.0.2 进入上游，不再属于当前差异。

以下三个文件已与上游 v1.0.3 同步：

- `VideoRenderNode.hpp`：原有自适应缩小采样，以及缩放时的独立单次采样材质、纹理过滤切换与 CPU 暂停帧保留。
- `FFmpegVideoPlayerQml.hpp`：默认开启的 `downsamplingEnabled` 与继承属性 `smooth` 的更新通知。
- `examples/Player.qml`：缩放状态绑定 `downsamplingEnabled`，保持 `smooth: true`。

从上游 v1.0.3 更新这些文件时，无需再次套用上述修复。此次同步只包含正式代码，未包含临时测试或性能诊断代码。

v1.0.2 过滤修复的设备测量见 [`doc/FFMPEG_PLAYER_FILTER_PERFORMANCE.md`](../../doc/FFMPEG_PLAYER_FILTER_PERFORMANCE.md)；其中缩放组使用最近邻，不能作为 v1.0.3 双线性缩放的性能结果。下面的画质记录描述原有普通模式降采样，本轮保持其采样数量、位置和颜色转换。

原 v1.0.1 只做一次 `GL_LINEAR` 采样且没有 mipmap：视频分辨率高于屏幕时，每个输出像素只采
2×2 纹素，高频折叠成摩尔纹——1080p 缩到 302×170 实测高频能量是正确缩放的 **13 倍**，
观感像整幅画面被"锐化"（360p 只缩小 2.1 倍，所以以前没被发现）。
已进入上游 `06c6e8f` 的降采样改动：把 `GlVideoMaterial`/`GlVideoShader` 的片元着色器改为按模式动态拼接，
缩小时沿 footprint 做 **N×N 加权抽样**（N = 缩小倍数，上限 External 8 / NV12 6 / I420 5，
放大时 `taps=1` 走原来的单次采样），并在 `GlVideoNode::UpdateTapParams()` 里由
`纹理尺寸 / 显示尺寸(devicePixel)` 算出 N 与抽样间距。保留 dma-buf 零拷贝与硬解。
参数变化时会打一行 `Video downsample: taps=…x… source=…x… display=…x…` 便于现场确认。

- 原有降采样段落的 `// PENMODS PATCH` 注释保留其来源，不代表当前仍是尚未回传的本地补丁。
- 实测（同一素材，拉普拉斯方差作高频指标，画面区 302×170）：1080p 修改前 **88 620** → 修改后
  **5 877**（`taps=7×7`；理想面积平均 6 739、设备上的 mpv/swscale 4 719）；720p 67 429 → 6 627（`taps=5×5`）；
  360p 62 851 → 14 772（`taps=3×3`）；256×144 放大时 `taps=1×1`，与上游单次采样相关性 0.992（无回归）。
- `FFPLAYER_GL=0` 仍可退回 CPU RGBA 路径。CPU 普通模式
  保持 swscale 先缩到显示尺寸，关闭降采样时改用原尺寸转换，显示过滤由 `smooth` 独立控制。

构建（`xmake f` / `xmake build` 会自动装依赖，不需要 `xmake require`）：

```sh
# 本子工程默认不参与工程解析（依赖链很重，见下表），需要 PENMODS_WITH_PLAYER=1 显式开启
PENMODS_WITH_PLAYER=1 xmake f -c <原有配置...> -y
PENMODS_WITH_PLAYER=1 xmake build ffmpegplayerplugin   # 产物 build/<plat>/<arch>/<mode>/qml/FFmpegPlayer/

# 只出纯软解版本（不链接 MPP/DRM）：
PENMODS_FFMPEG_PLAYER_SW=1 xmake f -c <原有配置...> -y && xmake build ffmpegplayerplugin
# 或者直接让 external/dictpen/librockchip_mpp.so.1 + libdrm.so.2 不存在
```

> ⚠️ 不要在 PenMods 工程里跑 `xmake require ffmpeg`（上游 README 这么写是因为它只在
> 自己的工程里构建）：该命令按 **recipe 默认值**（`network_revision = 7`、`rkmpp = false`）
> 再装一个 ffmpeg 变体，紧接着的 `xmake build` 就会链接这个软解变体，
> 表现为"配置里明明开了硬解，产物却没有 librockchip_mpp"。`xmake f` / `xmake build` 本身
> 已会按工程配置自动安装依赖，不需要 `xmake require`。

## ⚠️ 硬解与宿主门控（务必保留）

出厂固件的设备树把 VPU 关掉了（`/proc/device-tree/vpu_combo` = `disabled`），
此时 **厂商 MPP 会在 `mpp_init()` 里段错误**（`PC is at 0x0`），而 MPP 的能力预检
`mpp_check_support_format()` 仍然返回 0，所以进程内的播放器会直接崩掉整个宿主。
宿主的对策在 [`src/mod/Mod.cpp`](../../src/mod/Mod.cpp)：启动时如果没有
`/dev/vpu_service` 就 `qputenv("FFPLAYER_HWDEC","0")`，让插件走软解。

- 内核/设备树启用 VPU（`vpu_combo.status = "okay"`）之后，门控自动放行，插件会使用
  `h264_rkmpp`/`hevc_rkmpp`，`hardwareDecoding` 变为 true。
- 在任何**其它**进程里加载本插件（例如用 `qmlscene` 做冒烟测试）也必须自己设
  `FFPLAYER_HWDEC=0`，否则同样会崩。
- 注意设备上 `adbd` 的环境里可能已经带着 `FFPLAYER_HWDEC=0`（本次实测的机器就是），
  从 `adb shell` 起的进程会被它影响。

## 与上游同步

在 PenMods 根目录执行，`<upstream>` 指向上游 v1.0.3 的检出目录。先预览源码与示例的变化：

```sh
rsync -rlpc --dry-run --itemize-changes \
  <upstream>/src/ external/ffmpeg-player/src/
rsync -lpc --dry-run --itemize-changes \
  <upstream>/examples/Player.qml external/ffmpeg-player/examples/Player.qml
```

核对后移除 `--dry-run` 执行同步，再审阅 `git diff`。这些命令仅同步 `src/` 与示例，不删除目标额外文件，
也不覆盖 PenMods 的 `xmake.lua`、`UPSTREAM.md` 或本地构建产物。上游删除或重命名文件时，按差异单独处理。

`xmake.lua` 按上面的差异表单独合并：先比较旧、新上游版本，确认构建或依赖变化，再应用到 PenMods 的适配版本。
保留 `PENMODS_WITH_PLAYER` 门控、`path.join(os.projectdir(), …)` 路径基准、软解开关与 moc 构建适配。
向独立上游回传代码时同样保留其独立工程的 `xmake.lua`，不要整份互相覆盖。
`patches/` 与 `external/` 在上游确有变更时另行核对；v1.0.1 → v1.0.3 同步未涉及这两个目录。
若同步源是尚未包含回传修复的旧 tag 或提交，应先比较上述三个文件，保留当前共享实现。

## 构建与验证

```sh
PENMODS_WITH_PLAYER=1 xmake f -c --qt="<Qt 5.15.2 安装目录>" --arch=arm64-v8a \
  --build-platform=YDP02X --target-channel=dev --toolchain=zig -m release -vD \
  --cross=aarch64-linux-gnu.2.27 --force-debug-log=false -y
PENMODS_WITH_PLAYER=1 xmake build ffmpegplayerplugin
```

换依赖之后（例如 v1.0.0 → v1.0.1 的 `openssl3` → `ffplayer-openssl3`）第一次构建会重新下载/编译
OpenSSL 3.5.6 并重编 FFmpeg（约 3 min）；之后只改 `src/` 时，`xmake build ffmpegplayerplugin`
只重编插件本身（约 15 s）。

### ⚠️ 同步 `src/` 后必须全量重编插件

`moc_FFmpegPlayerPlugin.cpp.o` 由 `FFmpegPlayerPlugin.hpp` 生成——v1.0.2 改动了它间接包含的
`FFmpegVideoPlayerQml.hpp`，xmake 的增量依赖可能不重编它。结果就是一个 **混编 `.so`**：
同一个 `VideoPlayer` 在两个编译单元里布局不同（当前版本的 `Clock` 多了一个
`AudioTimeline` 成员）。

实测（2026-10-11，v1.0.0 → v1.0.1）：混编产物在 QML 创建 `VideoPlayer` 的瞬间
`libc++abi: terminating` + `Aborted`，**直接带走宿主进程**（对照：把 `src/` 全量重编后同一份源码
播放正常）。所以同步上游 `src/` 或依赖之后，先删掉插件的对象文件再编：

```sh
rm -rf build/.objs/ffmpegplayerplugin build/.gens/ffmpegplayerplugin \
       build/linux/arm64-v8a/release/libffmpegplayerplugin.so \
       build/linux/arm64-v8a/release/qml/FFmpegPlayer
PENMODS_WITH_PLAYER=1 xmake build ffmpegplayerplugin     # 期望看到 3 个 compiling 单元
```

注意 `xmake f -c` **只清配置缓存，不清 `build/` 里的对象文件**，不能用来代替上面的删除。
