# V2.7.0 / V2.7.1 启动故障修复与 V2.7.3 迁移

V2.7.0 和 V2.7.1 附带的更新启动器存在冷启动故障，可能导致无法进入应用、反复整机重启，随后被固件切回原厂系统。请停止安装这两版的迁移包和启动器，改用 [V2.7.3 完整迁移包](https://github.com/Lyrecoul/PenMods/releases/download/V2.7.3/update_v2.7.3.zip)。仅覆盖主库、资源库，或仅在新更新中心安装 core 包，不能更新独立启动器及 `/usr/bin/runDictPen` 入口。

问题与恢复记录见 [#32](https://github.com/Lyrecoul/PenMods/issues/32)。冷启动时，旧启动器先校验组件，推迟了原应用关闭硬件看门狗的时机；另外，入口失败可能被系统守护器放大为反复重启。V2.7.3 将看门狗交接提前，加入失败降级与 16 秒冷却，并在连续三次快速异常退出后尝试本次开机使用原厂备份。

## 一、还能进入系统，或仅更新中心提示启动入口未安装

先备份个人配置、聊天记录及需要保留的文件。电脑安装 ADB，连接字典笔，运行 `adb devices` 确认只有目标设备。以下命令在电脑执行；ZIP 从 V2.7.3 发布页下载到当前目录。

```sh
adb shell 'mkdir -p /userdisk/PenMods/install-2.7.3'
adb push update_v2.7.3.zip /userdisk/PenMods/install-2.7.3/
adb shell 'cd /userdisk/PenMods/install-2.7.3 && unzip -o update_v2.7.3.zip && sh _do_update.sh'
```

必须确认脚本成功退出；若报 `unrecognized or ambiguous runDictPen layout` 或备份不匹配，保留输出和 `/usr/bin/runDictPen` 及其备份供排查，不要强行替换任意行或伪造成功标记。

完整包会以临时文件加重命名替换独立 helper，暂存成对运行库并升级启动入口，保留已有配置和组件。成功后执行一次：

```sh
adb shell 'sync; killall -TERM YoudaoDictPen'
```

新版入口在应用退出后会等待 16 秒才交回系统守护器，因此重启应用比以前慢。等界面稳定，至少等待约一分钟再检查：

```sh
adb shell '/userdata/PenMods/penmods-update status'
adb shell 'grep "PenMods guarded launcher v3" /usr/bin/runDictPen'
```

应看到 core 为 2.7.3、`launcher_ready:true`、`pending:null`、`trial:null`，且没有新增 `last_error`。历史错误可能保留；若仍有 pending/trial，先等待并检查日志，不要连杀进程。随后在 **更多设置 → 软件更新** 将播放器更新至 **1.0.5**，下载暂存后再重启应用。也可先完成主程序迁移，再下载安装可选 MathJax 或 OCR。

最后正常关机开机一次验证冷启动。启动健康确认只证明应用完成初始化，不代表每个功能都已验收。

## 二、正在反复重启，但 ADB 仍可连接到正常系统

优先在可连接时直接执行上一节完整迁移步骤。若来不及完成，可以先恢复迁移前的启动脚本，绕过旧 helper；本步骤只适用于存在有效原始备份的正常系统。

先检查并把两个脚本保存到电脑：

```sh
adb pull /usr/bin/runDictPen runDictPen.faulty
adb pull /usr/bin/runDictPen.pre-penmods-update runDictPen.original-backup
```

用文本编辑器查看 `runDictPen.original-backup`：它必须是完整的原启动脚本，含 `stdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1`，不应含 `penmods-update` 或 `PenMods guarded launcher`。备份缺失、内容不符或不确定时停止本步骤，不要使用其他机型的脚本。

确认后执行（临时文件与重命名保留正在运行的旧 inode）：

```sh
adb shell 'mount -o remount,rw / && cp -p /usr/bin/runDictPen.pre-penmods-update /usr/bin/runDictPen.penmods-repair && sh -n /usr/bin/runDictPen.penmods-repair && sync && mv -f /usr/bin/runDictPen.penmods-repair /usr/bin/runDictPen && sync'
```

等待当前守护循环返回并重新启动应用；若系统仍重启，先保留日志进一步排查。恢复原入口后，PenMods 可能仍能运行，但更新中心暂时不能激活新版本。稳定后按第一节安装 V2.7.3 完整包，不能只停留在绕过状态后继续用 core 更新。

## 三、已经切回原厂系统，PenMods 消失

如果系统已自行还原，原启动脚本备份也可能消失。不要反复安装 V2.7.0 / V2.7.1 试图恢复。先按 [Installer](https://github.com/PenUniverse/Installer) 的说明重新安装 PenMods 底座，再执行第一节 V2.7.3 完整迁移步骤。迁移包负责更新库和启动器，不负责给已还原的原厂可执行程序重新添加 Mod 加载入口。

不要手动删除 `/userdisk/PenMods/update-system`、修改 active 指针或写系统槽来解决应用更新问题。代际存储不备份可变配置，个人数据需另行备份。

## 四、ADB 显示需 auth，且已进入 recovery

这与正常系统中的启动脚本修复不同：此时 `/usr/bin/runDictPen` 可能根本未挂载。不要在 recovery 直接执行前面的迁移或脚本恢复命令，也不要照搬别人的 misc 镜像。先保留设备状态与日志，参考仓库 [恢复说明](../AGENTS.md#recovering-from-a-crash-loop--recovery)，使用适用于自己设备且已验证的恢复材料，或向维护者提供机型和故障输出。恢复正常系统后再进行第一节迁移。

## 验证边界

#32 的修复已有 YDP02X 双槽冷启动记录；V2.7.3 发布构建另做主机故障注入、入口安装和打包测试。本次发布时没有连接设备，未重新执行 V2.7.3 整包实机迁移、冷启动及播放器验收。原厂降级依赖有效的 `YoudaoDictPen.original_bak` 或 `.bak` 和可运行的 helper；它不能恢复缺失的原厂备份，也不能检测应用永久卡死。
