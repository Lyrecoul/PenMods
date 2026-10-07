# external/ffmpeg-player —— 上游代码与本仓库的改动

本目录是 [`Haikure/ffmpeg-player`](https://github.com/Haikure/ffmpeg-player) 的 **vendor 副本**
（面向 RK3326 / AArch64 / Qt 5.15 的 QML 视频播放器插件），用来给 PenMods 提供
「画在 Qt 场景图里」的视频播放能力（跟随左右手旋转、画面内控件、ASS/LRC 字幕）。

- 上游版本: **v1.0.0**（commit `48ca849`，`feat: improve player controls and TLS trust setup`）
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

## 与上游的差异（全部集中在 `xmake.lua` 和本文件）

| 改动 | 原因 |
|---|---|
| 去掉文件级 `add_rules("mode.debug","mode.release")`；把 `set_languages("c11","c++17")`、`set_optimize("fastest")`、`set_warnings("all","error")` 移进 target 作用域 | 本文件现在由根工程的 `includes()` 载入，文件级设置会污染 PenMods 自己的 target（它是 c++23、且不能把警告当错误） |
| target 内加 `set_pcxxheader()` | 根工程给所有 target 预设了 PCH `src/base/Base.h`，插件不该用它 |
| 所有 `os.scriptdir()` / 相对路径 → `path.join(os.projectdir(), "external", "ffmpeg-player", …)` | 被 `includes()` 载入后，相对路径的基准是根工程目录 |
| `add_deps("openssl3 3.5.7")` → `3.5.6` | xmake-repo 里没有 3.5.7；按本机已有的版本钉住（上游若更新可跟随调整） |
| `network_revision = 7` → `8` | 该字段参与 ffmpeg 包哈希，改它可强制重编；也用于让本地旧变体失效 |
| 文件开头加"默认跳过"开关：没有 `PENMODS_WITH_PLAYER=1` 就 `return`，连 `add_requires` 都不声明 | CI 与只编 libPenMods.so 的人不必为一个 40 分钟的依赖链（FFmpeg/OpenSSL3/libass/harfbuzz→meson→python）和容器里的 `unzip`/`perl` 买单；`xmake build` 仍会成功（插件被跳过并打印一行提示） |
| `rkmpp = has_device_libs(...)` → `rkmpp = use_rkmpp`（设备库存在即开启；`PENMODS_FFMPEG_PLAYER_SW=1` 可强制软解） | 保留原语义、去掉隐式行为；不用 xmake option 是因为本机 xmake 在工程加载阶段 `get_config()` 读不到 CLI 传进来的值（实测为 nil） |

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

```sh
# 在上游仓库取新版本，然后覆盖回来（只保留 xmake.lua 的适配与本文件）
rsync -a --delete --exclude build --exclude .xmake \
  <upstream>/ external/ffmpeg-player/
# 再按上面的差异表重新套用 xmake.lua 的改动
```
