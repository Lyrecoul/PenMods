# PenMods
> 一般来说，你不需要手动下载 PenMods  
> **全新安装** 👉[Installer](https://github.com/PenUniverse/Installer)  
> **从旧版更新** 👉系统自带的 OTA

这里是 PenMods[^1] 的发布仓库。

## Building

### Requirements

- Linux 环境（以下以 x86_64 架构机器为例）
- 安装 [gcc aarch64 6.5 编译器](https://github.com/Redbeanw44602/aarch64-linux-gnu-gcc-6.5.0)，Archlinux 系列发行版可以直接安装，其它系统需要自行解压和部署到系统 PATH 环境变量，还需要安装 zig 工具链
- 部署 [Qt 开发框架](https://github.com/Redbeanw44602/aarch64-linux-qt-5.15.2)，需要参考 README 中的方法
- 安装 xmake、git、zig 等必须开发环境依赖包（新版 xmake 工具链调用参数有变化，以下均采用仓库内提供的旧版作示例）

### Xmake
```shell
# 配置 xmake（以下命令需要根据自己的情况修改，只是示意参考）
xmake f # 等价于 xmake config ... \
  --qt="/home/example/PenMods/aarch64-linux-qt-5.15.2" \
  --arch=arm64-v8a \
  --build-platform=YDP02X \
  --target-channel=dev \
  --toolchain=zig \
  -m debug \
  -vD \
  --cross=aarch64-linux-gnu.2.27 # 必须设置，用以保持与词典笔系统 glibc 版本的兼容性 \
  --force-debug-log=true # （可选）如果需要获得带 debug 级别日志的构建 \
  -c # 配置过之后可能出现过了 00: 00 zigcc 工具链找不到的问题，需要加这个参数清理配置缓存
xmake config --menu #（可选）xmake 图形化配置菜单，可以进一步微调各种编译选项

# 编译项目
xmake build # 开始编译所有目标
xmake build -v PenMods # 只编译 PenMods 库（启用 -v 选项输出详细编译过程信息）
xmake build PenModsResources # 只编译 qrc 资源库（放在 libPenMods.so 同一目录）
xmake build QrcExporter # 编译 Qrc 资源导出模块（需要手动 patch 有道词典笔主程序并且在主程序重启后长按按键触发）

# 其它
xmake clean # 清理编译产物
```

### Supported
 - 设备支持情况
 - 对于同代（按屏幕大小区分代）但不同发行版本较多的笔，不一一适配，仅适配功能最多的型号
 - 词典笔 OS[^2]与目前的经典系统相差太大，目前的 Mod 不可能适配
 - 根据有道官方消息，YDP03x（三代）有计划全面升级到词典笔 OS

🟢 | 测试通过 | 🟡 | 适配工作正在进行 | 🔴 | 暂无计划
-|-|-|-|-|-

型号 | 是否支持 | 系统版本 | Mod 版本 | 备注
-|-|-|-|-
YDP02x | 🟢 | 2.0.7, 2.0.8 | Latest | 老系统请先自行刷到新系统，请保证 `userdisk` 分区可以正常读写
YDPG3 | 🟢 | 1.0.2, 1.0.3 | 1.1.8 | -
YDP03x | 🟡 | 2.7.2 | - | -
P5/X5 | 🔴 | N/A | N/A | -

### Notice
 - **PenMods 会拦截原系统更新**，若原系统有更新，需要先卸载 PenMods 才能更新（因为版本更新可能导致 Mod 不兼容或其他不可预料的情况）
 - **安装 PenMods 可能导致您失去有道官方保修**
 - 使用 PenMods 造成的一切后果均由您本人承担，与项目作者没有任何关系

### Features (in this repository)
 - 重新实现的 AI 助手功能
  - 兼容 OpenAI 格式 API
  - 可任意配置多个模型和提示词
  - 可使用简单的 Tool Call（shell 命令执行和 Tavily 网络搜索 API）
  - 数学公式渲染（需要配置 MathJax 数学公式渲染服务器）
  - 可引用本地文件
 - 增强文件管理器功能
  - 添加图片查看器（另外添加 Webp 图像查看支持，包括动图）
  - 添加外部的视频播放器支持（执行 /userdisk/VideoPlayer \[视频文件路径\] 打开播放器）
  - 支持查看隐藏文件（可被安全锁功能保护）
  - 支持自然排序（按数字大小排序）
 - 简单的插件系统
  - 可以在 /userdisk/PenMods/plugins 目录下创建目录作为插件执行自己的小程序（以 QML 为主，原生运行库为辅）
  - TTS 功能 QML 接口包装
  - Shell 执行 QML 接口包装
 - 主界面的壁纸功能
 - 增强音乐播放器
  - 添加 lrc 翻译歌词自动转换功能
  - 使用临时软链接使得自带播放器能够直接打开 flac 等格式音乐文件
 - 键盘的中州韵输入法支持
  - 在键盘页面长按“abc”按钮可触发拼音模式，可进行简单的输入
  - 输入方案可自定义（需要放在 /userdisk/Music/Rime 目录下）
 - 音频守护进程
  - 接管有道主程序原生输出管理机制，可以在 /tmp 目录下创建音频唤醒锁避免输出关闭
 - Github Action CI 自动构建配置

### Features (1.2.0)
 - 增强 AI 助手
   - New Bing [配置指南（已失效）](https://dictpen.amd.rocks/post/126)
 - 增强单词本
   - 支持区分词组和单词（可开关）
   - 修复 “选择语言” 不会记住的 Bug
   - 单词间相互大小写不敏感（可开关）
   - 导出更详细的内容
 - 增强 “我的导入”
   - 重新实现文件管理：完全重写实现逻辑，不再依赖 SQLite，性能提升
   - 重新实现自动播放：完全重写随机播放等逻辑
   - 增强系统播放器，增加有效音频检查，并在任何场景下都可用倍速和复读
   - 文件管理支持无限级子目录
   - 文件管理器现支持删除/改名/排序
   - 文件管理器可以打开 `txt`、`md`（markdown）文件
 - 增强查词功能
   - 查词支持手动输入（英文，可开关）
   - 扫描查询支持将扫描结果转到小写（可开关）
 - 息屏控制
   - 添加自动息屏时间控制：30 秒，1,2,3,4,5 分钟，永不
   - 添加场景智能息屏（音频播放器和单词本卡片模式不会自动息屏，可开关）
 - 电池信息查看/休眠控制
   - 添加电池信息：电池状态、电压、温度、健康状况、实时电流
   - 支持控制自动休眠时间：5,10,20,40 分钟，永不
 - 安全选项
   - 可自设密码，目前有：重置选项页、开发者选项页
   - 防止尴尬系列：蓝牙断连自动静音，强制禁用自动发音/朗读，降低最低音量
   - 默认密码 `abcd`
 - 防止未经许可的日志上传
   - 清除系统日志
   - 防止未经许可的日志上传（行为记录，扫描图像等）
 - 开发者选项
   - 支持管理 ADB/SSH 服务（目前这个的功能比较简单）
   - 在 WiFi 连接页面显示当前词典笔的局域网 IP 地址
 - 录音机
   - 录音自动保存到 /userdisk/Music/录音文件
 - 手电筒
   - 可在设置内自行开关笔头灯
 - 键盘
   - 键盘已支持多行输入
   - 在键盘页面扫描，扫描结果将附加到光标处
 - 其他
   - 支持控制列式数据库单次查询限制（比如单词本原版一次只加载 10 个...）
   - 切换系统 A/B 槽位
   - 快速静音
   - 开机自动挂载根目录为可读写
   - 内置自动 vendor_storage 修复
   - OTA 更新 Mod

### Installation
 - 请移步 [Installer](https://github.com/PenUniverse/Installer)

### Contact Us
 - [官方社区](https://github.com/orgs/PenUniverse/discussions)
 - Telegram 群组 [PenUniverse](https://t.me/PenUniverse)

> 请注意：PenMods 没有创建任何官方 QQ 群。

### Credits
 - [Dobby](https://github.com/jmpews/Dobby)
 - [Qt Project](https://www.qt.io/)
 - [injector](https://github.com/kubo/injector)

### Donation
 - 如果你觉得 PenMods 做的不错，请考虑捐赠 👉[爱发电](https://afdian.net/a/kbs007)
