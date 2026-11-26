# PenMods

PenMods 是面向网易有道词典笔经典系统的运行时扩展，为设备提供 AI 助手、文件与媒体管理、查词增强和系统设置等功能。

当前主要适配 **YDP02X**。其他型号与固件的兼容性请以对应版本的发布说明为准。

## 主要功能

- **AI 助手与翻译**：支持兼容 OpenAI 格式的 API、多模型配置、图片输入、工具调用和数学公式渲染。翻译结果可带上原文和译文向 AI 追问。
- **文件与媒体**：文件管理、文本阅读、图片查看、音乐与视频播放，以及歌词和字幕支持。
- **学习工具**：查词与单词本增强、自定义词典、循环朗读和录音。
- **输入与界面**：Rime 输入法、键盘布局切换和自定义壁纸。
- **系统设置**：熄屏与休眠控制、电池信息、蓝牙耳机电量和安全锁。
- **插件扩展**：支持 QML 与原生插件，可扩展界面、系统接口和媒体控制。

部分功能需要配置外部服务或安装可选组件。

AI 助手的 PDF 附件需要模型支持。可在模型设置中为支持 PDF 输入的接口开启「PDF 附件」，或使用同一开关启用官方 `deepseek-flash` 的文件解析。

## 安装与更新

从 [Releases](https://github.com/Lyrecoul/PenMods/releases) 获取发布包，并阅读对应版本说明。

- **已使用新版更新中心**：在「更多设置 → 软件更新」中管理主程序、视频播放器、MathJax 和 PP-OCRv5。更新暂存后，在下次启动应用时生效；启动验证失败会自动回退。
- **从旧版迁移，或提示更新启动器未安装**：按[迁移说明](doc/UPDATE_SYSTEM.md#手动安装迁移包)执行完整迁移包。
- **原厂系统首次安装**：需要匹配设备与固件的底座安装方案；完整迁移包不提供首次安装功能。

> **V2.7.0 / V2.7.1 用户**：请先阅读[启动故障修复与迁移教程](doc/RECOVERY_2.7.3.md)。仅更新 core 或替换动态库无法修复旧启动器。

## 使用须知

- 安装前请备份个人数据，并确认设备型号、系统版本与发布包匹配。
- PenMods 会拦截原系统更新；更新原系统前请先卸载 PenMods。
- 安装可能影响官方保修。修改系统存在风险，请自行评估并承担使用后果。

## 开发与维护

### 代码入口

- [`src/mod/Mod.cpp`](src/mod/Mod.cpp)：进程初始化与各模块的启动顺序。
- [`src/mod/Engine.cpp`](src/mod/Engine.cpp)：QML 上下文注册和资源加载；[`src/common/Event.h`](src/common/Event.h) 提供生命周期事件。
- [`src/base/`](src/base/)：Hook、符号解析与宿主类型；其他功能按模块放在 `src/` 下。
- [`src/mod/Config.cpp`](src/mod/Config.cpp)：配置默认值与迁移。设备配置文件为 `/userdata/PenModsconfig.json`。

### 构建

需要 Linux、Git、xmake、Zig、AArch64 交叉工具链（glibc 2.27）、目标端 Qt 5.15.2 SDK，以及主机可运行的 Qt 工具（如 `moc`、`rcc`）。依赖安装和工具链版本可参考 [CI 配置](.github/workflows/build.yaml)。如需安装器源码，先运行 `git submodule update --init --recursive`。

以下以 YDP02X / dev 为例，在仓库根目录执行，并将 `--qt` 改为本机 SDK 路径：

```sh
xmake f \
  --qt="$HOME/PenMods/aarch64-linux-qt-5.15.2" \
  --arch=arm64-v8a \
  --build-platform=YDP02X \
  --target-channel=dev \
  --toolchain=zig \
  -m release \
  --cross=aarch64-linux-gnu.2.27 \
  --force-debug-log=false \
  -c

xmake build PenMods          # 主库 libPenMods.so
xmake build PenModsResources # 资源库 libPenModsResources.so
xmake build PenModsUpdate    # 独立更新启动器 penmods-update
```

- `--cross=aarch64-linux-gnu.2.27` 用于保持设备 glibc 兼容性，不要省略。
- 调试时可启用 `--force-debug-log=true`；发布构建应关闭。若缓存的 `/tmp` Zig 编译器包装器丢失，重新执行配置命令。
- 产物默认位于 `build/linux/arm64-v8a/release/`。不要直接执行 `xmake run`，其部署脚本含有开发者本机路径。
- FFmpeg 播放器需设置 `PENMODS_WITH_PLAYER=1` 后单独构建 `ffmpegplayerplugin`；MathJax 与 PP-OCR 也有独立构建流程。播放器依赖和完整重编要求见[集成说明](external/ffmpeg-player/UPSTREAM.md)。

### 修改 QML 资源

YDP02X 的可编辑源码位于 `resource/models/YDP02X/qml/`，界面需适配 **320×170**。源码树未随仓库跟踪，新克隆需先从已提交的资源头还原：

```sh
scripts/unpack_qt_res.sh YDP02X
```

修改后重新生成资源，并将生成的 `resource/models/YDP02X/qrc_qml.h` 随改动提交；不要手工编辑该文件：

```sh
(cd scripts && sh gen_qt_res.sh YDP02X)
xmake build PenModsResources
```

设备优先加载外置资源库。只更新主库会留下旧界面，发布或制作 core 包时必须包含匹配的主库和资源库。

### 本地验证与部署

- 先完成受影响目标的构建，再运行相关模块的主机检查；例如更新打包工具使用 `python3 tools/test_package_update.py`，启动器检查见 [`tools/update/README.md`](tools/update/README.md#host-checks)。
- 已安装更新启动器的设备应将成对运行库打包，通过 `penmods-update stage` 暂存后重启应用。测试前保存原版本的可恢复包，完成后可重新暂存原包恢复；具体打包方式见[更新系统说明](doc/UPDATE_SYSTEM.md#制作数据包)。
- 不要覆盖正在运行的动态库，也不要直接修改受管版本目录或 `active` 指针。没有受管更新系统的手动部署也应先上传临时文件，再重命名并重启应用。
- 重启后检查 `/userdata/applog/DictPen_*.log` 中的 QML 错误，并验证实际界面；设备截图方法见[截图通道](doc/SCREENSHOT_CHANNEL.md)。启动健康检查通过不代表功能验证通过。

### CI 与发布

[构建工作流](.github/workflows/build.yaml) 按构建输入路径触发，可手动运行。仅改文档、`mathjax-server/` 或 `tools/update/` 以外的工具不会触发该构建；新增构建输入时需同步更新 `push` 与 `pull_request` 的路径过滤。播放器默认不构建，手动触发时可选择启用。

版本号统一维护在 [`xmake.lua`](xmake.lua) 的 `set_version(...)`。CI 上传构建产物，不会自动发布 Release 或更新 OTA 清单；发布前需核对主库、资源库和可选组件是否匹配，并验证更新、重启和失败回退流程。

## 文档

- [更新中心与迁移](doc/UPDATE_SYSTEM.md)
- [自定义词典](doc/CUSTOM_DICT_GUIDE.md)
- [插件开发](doc/PLUGIN_DEV_GUIDE.md)
- [原生插件 Hook API](doc/PLUGIN_HOOK_DEV_GUIDE.md)
- [项目架构与逆向分析](doc/REVERSE_ENGINEERING.md)

## 问题反馈

请在 [Issues](https://github.com/Lyrecoul/PenMods/issues) 提交问题，并附上设备型号、系统版本、PenMods 版本、复现步骤及相关日志。

## 许可与致谢

本项目采用 [GNU GPL v3](LICENSE) 许可。感谢 [Qt](https://www.qt.io/)、[Dobby](https://github.com/jmpews/Dobby) 等开源项目及所有贡献者。
