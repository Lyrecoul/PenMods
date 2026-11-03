# PenMods 更新中心

更新中心将主程序和可选扩展作为独立组件管理。下载首先校验大小和哈希，再交给独立原生启动器暂存。暂存产生待激活 generation；当前进程不切换版本，启动器在下次启动应用时激活。

## 组件与发布清单

组件 ID 固定为 `core`、`player`、`mathjax` 和 `ppocr`。清单继续保留旧 `status:200` 和 `versions`，扩展信息加入可选 `components` 数组：

```json
{
  "status": 200,
  "versions": ["existing legacy entries remain here"],
  "components": [{
    "id": "player",
    "name": "视频播放器",
    "versions": [{
      "version": [1, 2, 0],
      "size": 123,
      "unpacked_size": 456,
      "sha256": "64 lowercase hex characters",
      "download": "https://example/player.zip",
      "model": "YDP02X",
      "arch": "aarch64",
      "channel": "stable",
      "requires": {"core": {"min": "2.6.3", "max_exclusive": "3.0.0"}}
    }]
  }]
}
```

以上是结构示例，发布时必须填写真实大小、哈希和版本对象。扩展与新组件目录必须有 SHA-256；旧 core 的 MD5 信息仍可读取。core 目标版本遵守旧 Updater 的 `next` 升级路径。新 core 包必须声明 `health_protocol:1`，并同时包含主库和资源库。缺少心跳协议的旧包仍显示版本信息，但不能进入新启动器的候选试运行。

## 制作数据包

```sh
python3 tools/package_update.py --component player --version 1.2.0 --source build/player-package --output build/player.zip --descriptor build/player.json --download https://example/player.zip --model YDP02X --channel stable
```

[打包脚本](../tools/package_update.py)生成固定时间戳、稳定顺序和压缩方式的 ZIP。标准输出是可放入 `components[].versions[]` 的 JSON（version 为数组），`--descriptor` 保存可直接传给 native `stage` 的 JSON（version 为字符串）。正常数据包只复制规定的 payload，不打包安装脚本。core 元数据也输出 MD5，以兼容旧 `versions` 校验。`size` 与哈希对应完整 ZIP，`unpacked_size` 对应文件解压总大小。输入不能含软链接或特殊文件；库和程序必须是 little-endian aarch64 ELF。单文件最多 192 MiB，ZIP 最多 256 MiB，解压总量最多 512 MiB。

| ID | ZIP 内容 |
| --- | --- |
| core | 根目录 `libPenMods.so`、`libPenModsResources.so` |
| player | `FFmpegPlayer/libffmpegplayerplugin.so`、`FFmpegPlayer/qmldir` |
| mathjax | 根目录 `mathjax-server`，固定可执行权限 |
| ppocr | 根目录 `libyocr.so`，必须含 `models/PP_OCRv5_mobile_rec.ncnn.param` 和 `.ncnn.bin`；附加模型也必须成对 |

要求写入单独 JSON 文件，再以 `--requires FILE` 传入。model、arch、channel 与组件依赖由 native helper 再次验证；只有兼容组合才能成为待激活 generation。发布 URL 必须使用 HTTPS；版本化 CDN 路径发布后不要覆盖。

## 旧设备首次迁移

旧客户端尚无独立启动器时，可由同一发布工具生成一次性的兼容外包，之后使用正常数据包。正常 core ZIP 与兼容外包必须使用不同文件名和发布 URL：

```sh
python3 tools/package_update.py --component core --version 2.7.0 --source build/core-package --output build/core.zip --descriptor build/core.json --download https://example/core.zip --model YDP02X --channel dev --bootstrap-helper build/penmods-update --bootstrap-installer tools/update/install-launcher.sh --bootstrap-output build/bootstrap.zip --bootstrap-download https://example/bootstrap.zip --bootstrap-descriptor build/bootstrap.json
```

兼容外包包含 `_do_update.sh`、`penmods-update`、`install-launcher.sh`、`core-package.zip`、`core-descriptor.json`。迁移脚本依次：原子安装 helper；以 `init --snapshot-only` 快照旧程序（版本记为未知）；校验并暂存内包；运行严格识别原启动命令的入口安装脚本；最后创建新成功标记。快照或 stage 失败时不修改启动入口。入口安装失败时 pending 仍未激活，原程序路径不变。迁移不会覆盖已映射的主库或资源库。

同一清单中，旧 `versions` 对应该外包的大小、MD5 和 URL；`components` 中 `id:"core"` 的对应版本则指向正常内包。新中心选择相同 Updater 目标版本的 core 组件描述，不执行兼容外包中的脚本。发布者必须保留旧设备当前版本和 `next` 路径；核心新库必须真实实现健康协议。

迁移脚本支持 `PENMODS_BOOTSTRAP_PREFIX` 测试根目录，仅用于主机测试；正式设备不设置。正常包和兼容外包生成工具不进行网络发布。发布清单必须同时包含独立组件目录；未提供扩展元数据的自定义清单仍显示“暂无可用版本”。

## 手动安装迁移包

已有 PenMods 底座、但曾只替换动态库的设备，需要额外运行完整 OTA 包一次来安装独立更新启动器。新版页面提示“更新启动器尚未安装”时也使用此方法；单独下载主库和资源库无法补齐它。V2.7.0 / V2.7.1 的启动器存在冷启动故障，已安装的设备也应手动执行一次新版完整包，参见[修复教程](RECOVERY_2.7.3.md)。迁移包不负责原厂系统首次安装。

从[对应 Release](https://github.com/Lyrecoul/PenMods/releases/tag/V2.7.3)下载 `update_v2.7.3.zip`，连接 ADB 后执行：

```sh
adb shell "mkdir -p /userdisk/PenMods/install-2.7.3"
adb push update_v2.7.3.zip /userdisk/PenMods/install-2.7.3/
adb shell "cd /userdisk/PenMods/install-2.7.3 && unzip -o update_v2.7.3.zip && sh _do_update.sh"
```

命令成功后更新只处于待生效状态，回到软件更新页面刷新并重启应用即可；缺少新版页面的旧客户端可通过正常关机开机生效。不要直接覆盖正在运行的动态库。路径和启动脚本不匹配时迁移会报错并停止，不应手动伪造成功标记。

卸载后重装时，迁移先以 `init --reinstall-snapshot` 将当前安装的库重新快照为基线，清空旧 pending/trial，但保持实时别名与停用标记；只有新包暂存和启动入口安装均成功后才通过 `enable` 重新启用事务。失败时保持旁路启动。

## 连续崩溃后的原厂安全模式

v3 启动钩子独立于 Mod 记录快速异常退出：helper 返回异常退出码且运行不足 60 秒时加一，连续 3 次后立即运行同目录的原厂备份。正常退出、主动 HUP/INT/QUIT/TERM 重启，或运行满 60 秒后的退出会清零。该时长包含组件校验；内部候选回滚成功继续运行时不计一次失败。

安全模式通过 `/tmp/penmods-safe-mode` 保持到本次关机；整机重启后重新尝试 Mod。计数 `/tmp/penmods-launch-failures` 与厂商 guardian 的计数独立，原有 16 秒退出冷却仍有效。安全模式不改系统槽位、应用文件、组件别名或持久 disabled 标记。

原厂路径使用 `penmods-update stock -- APP ARGS...`，优先选择 `APP.original_bak`，其次 `APP.bak`；要求为可执行普通文件、AArch64 ELF、大小不超过 32 MiB，且文件内没有 `libPenMods` 字符串。链接、脚本、仍带 Mod 补丁的备份被拒绝；这不是原厂文件签名认证。启动时清除预加载和更新心跳变量，并排除 `/userdisk/Qtlib` 等 Mod 库覆盖路径，绕过可选 OCR。备份不可用时保留安全模式与冷却并记录错误，不转回崩溃的 Mod。

该功能覆盖应用快速退出，不检测运行卡死或整机掉电，也依赖 helper 自身可运行。安装需同时更新独立 helper 并重新运行新版 `install-launcher.sh`；只更新 core 无法补齐。安装器支持严格验证后升级 v1/v2 钩子，并保留原启动脚本备份。

## 启动器与事务

启动器路径为 `/userdata/PenMods/penmods-update`，存储根目录默认 `/userdisk/PenMods/update-system`。所有命令支持 `--root ROOT`；主机测试还可用 `--prefix PREFIX`。

- `status` 输出 JSON：`installed`、`active`、`pending`、`trial`、`components`、`pending_components` 与 `last_error`。组件信息为 `{version,path}`。
- `stage DESCRIPTOR_JSON ARCHIVE_ZIP` 再次校验哈希和 ZIP，安全解压到新目录，合并到已有 pending generation。不会激活。
- `cancel` 只删除待激活事务，不能取消已开始的试运行。
- `run -- APP ARG...` 导入初始版本，迁移兼容别名，并在启动前选择 generation。

active 指针同时选择主库、资源库与可选组件，避免跨版本混用；旧 generation 保留供回滚使用。下载和解压不会覆盖任何已映射的库。ZIP 先检查中央目录，拒绝路径穿越、软链接、设备、重复、加密、越界文件；不执行包内脚本。

启动器向子进程提供 `PENMODS_UPDATE_ROOT`、`PENMODS_UPDATE_GENERATION` 和 `PENMODS_UPDATE_TOKEN`。UI 完成后，UpdateCenter 每两秒通过 QSaveFile 原子写入 `health/TOKEN.json`：

```json
{"generation":"generation-id","token":"random-token","pid":1234,"sequence":7}
```

启动器绑定实际子进程 PID 和随机 token；递增心跳持续 20 秒才确认。120 秒内未达到条件或进程退出时回滚到上次已确认版本。候选无法加载本身也由独立进程监控，不依赖候选库执行回滚。确认完成后客户端发现 trial journal 已清除便停止心跳，避免持续写入闪存。userdisk 不可用时，启动器可恢复 userdata 下保留的基线主库和资源库别名作为应急启动路径。

## 页面与自动下载

`updateCenter.components` 始终提供四个组件，字段为 `id/name/installedVersion/availableVersion/note/sizeText/stateText/progress/available/installed/pending/busy/autoUpdate/canDownload`，另外有供诊断的 `state/error`。页面按 `canDownload` 决定安装或更新是否可操作。

`downloadComponent(id)` 完成下载、校验和暂存。各组件请求按队列串行执行，每项保存请求时的清单快照。取消下载会取消活动网络请求和后续队列，正在执行的 native 暂存则完成后退出。`cancelPending()` 丢弃待激活版本，`restartNow()` 重启应用以激活。清单源与代理继续使用原 Updater；`refresh()` 仅刷新本地状态，不重复发起网络清单检查。

`updater.auto_check` 默认 true；UI 完成 30 秒后开始调度，24 小时内的已接受网络清单可复用，过期会重新拉取。周期定时器持续检查到期时间；网络失败采用 5、10、20、40、60 分钟上限退避，恢复网络时也检查是否到期。缓存加载不重置原获取时间。`auto_download` 默认 false。`component_auto` 默认只为 core 启用。自动下载只更新已安装、明确选择的组件，不会自行安装未装扩展，也不下载只有 MD5 的旧 core 包。每个 SHA-256 在一次进程生命周期内最多自动尝试一次，失败后保留手动重试。自动下载只暂存，不主动重启。

导入的旧扩展版本显示 `0.0.0`（未知），不能当作已是最新版。安装和启用分别处理：播放器激活后由视频打开流程自动选择内嵌模块；若 QML 插件导入失败则回退到 mpv；OCR 组件详情可直接进入实验性引擎开关；MathJax 组件详情可进入公式渲染设置，公式服务路径固定使用活动 generation 中已安装的组件。旧配置中的 `server_path` / `server_source` 会被忽略并清理。聊天公式图片仅在完全气泡模式下显示。

MathJax 的聊天与文本阅读入口统一调用 `ChatBot::startMathServer()`，由同一个运行器持有服务进程，绑定应用生命周期。服务只从活动 generation 的已安装组件启动；迁移时仅根据 `/proc` 的可执行文件与进程启动身份回收明确属于旧配置的旧公式服务或旧受管版本，不使用进程名匹配。

## 验证范围

Native helper 的主机故障测试覆盖事务、ZIP 拒绝、崩溃、健康确认、损坏日志、首次启动救援及启动入口安装。[打包回归测试](../tools/test_package_update.py)使用临时目录覆盖四种布局、确定性、哈希、非法输入及旧客户端迁移成败路径。运行 `python3 -B tools/test_package_update.py`；设置 `PENMODS_UPDATE_TEST_HELPER` 为已构建的 host helper 路径后，共运行 9 项测试，包含真实 native 首次快照、暂存和卸载后重装集成。

2026-10-31 在 YDP02X、Qt 5.15.2、glibc 2.27 上完成启动器、主库、资源库交叉构建和实机迁移验证：安装迁移包后旧进程和两库哈希保持不变；重启后激活候选，UI 心跳完成确认；已安装 player/OCR 保持原状态，配置与聊天记录沿用原路径。实机隔离根目录另外验证了候选退出 42 后自动回退，以及递增心跳确认；没有故意让真实字典应用崩溃。已在设备进入更新中心、播放器详情、更新设置和重启确认页。

2026-11-01 在同一 YDP02X 使用系统 Qt 从 `/userdata/PenMods/qml` 导入活动 FFmpegPlayer 模块并播放 `/userdisk/Music/aa_clip.mp4`：创建了 VideoPlayer，日志确认 h264_rkmpp 解码及 OpenGL 零拷贝渲染；活动插件哈希与本地 2.7.0 发布包一致。此项验证证明受管模块可真实加载和播放，但尚未通过文件管理器页面验证应用内 Loader 流程或 mpv 错误回退分支；OCR 扫描与 MathJax 渲染仍按各组件发布内容单独测试。

目前使用 HTTPS 与大小/SHA-256 校验保证下载传输和文件完整性，尚未配置发行公钥和签名验证；不能将清单中的哈希等同于发布者签名。
