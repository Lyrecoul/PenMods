# PenMods

> 这里是 PenMods 的发布仓库。

## Building

### Requirements

- Linux 环境
- 安装 [编译器](https://github.com/Redbeanw44602/aarch64-linux-gnu-gcc-6.5.0)，Archlinux 系列发行版可以直接安装，其它系统需要自行解压和部署到系统 PATH 环境变量
- 部署 [Qt 开发框架](https://github.com/Redbeanw44602/aarch64-linux-qt-5.15.2)，需要参考 README 中的方法
- 安装 xmake、git 等必须开发环境

### External

- [Dobby-0.1.2](https://github.com/Redbeanw44602/Dobby)
- [LAME-3.100](https://github.com/despoa/LAME)


### Xmake
```shell
# 配置 xmake
xmake f --qt="/home/lyrecoul/PenMods/aarch64-linux-qt-5.15.2" --arch=arm64-v8a --build-platform=YDP02X --target-channel=stable --toolchain=cross-aarch64 # 等价于 xmake config ...
xmake config --menu #（可选）xmake 图形化配置菜单，可以进一步微调各种编译选项

# 编译项目
xmake build # 开始编译所有目标
xmake build -v PenMods # 只编译 PenMods lib（启用 -v 选项输出详细编译过程信息）
xmake build QrcExporter # 编译 Qrc 资源导出模块
xmake build dobby_lib # 编译 Dobby 静态链接库
xmake build lame_lib # 编译 LAME 静态链接库

# 其它
xmake clean # 清理编译产物
```

### Supported
 - 设备支持情况
 - 对于同代（按屏幕大小区分代）但不同发行版本较多的笔，不一一适配，仅适配功能最多的型号
 - 词典笔 OS 与目前的经典系统相差太大，目前的 Mod 不可能适配

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

### Features (1.3.0)
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
   - 添加自动息屏时间控制：30秒，1,2,3,4,5 分钟，永不
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
   - 在WiFi连接页面显示当前词典笔的局域网 IP 地址
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
