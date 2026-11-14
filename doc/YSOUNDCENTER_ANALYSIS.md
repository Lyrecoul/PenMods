# YSoundCenter 逆向分析报告

> 分析目标：二进制中的 `YSoundCenter` 类（QObject 子类）  
> 分析方法：IDA MCP Server — 元数据解析 + 符号表分析  
> 分析时间：2026-05-23

---

## 1. 类基本信息

| 项目 | 内容 |
|------|------|
| **类名** | `YSoundCenter` |
| **基类** | `QObject` |
| **命名空间** | 全局（无命名空间） |
| **虚函数表** | `0xb6dc10` |
| **元数据（字符串区）** | `0xa0b540` |
| **元数据（索引区）** | `0xa0b4e0` |

---

## 2. 继承层次

```
QObject
  └── YSoundCenter
```

---

## 3. 属性 (Properties)

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `pos` | `qint64` | 音频播放当前位置（毫秒） |
| `duration` | `qint64` | 音频总时长（毫秒） |
| `currentPlayId` | `QString` | 当前播放内容的唯一标识 ID |
| `state` | `QProcess::ProcessState` | 声音进程状态：`NotRunning` / `Running` / `Suspended` |

### 属性 NOTIFY 信号映射

| 属性 | NOTIFY 信号 |
|------|------------|
| `pos` | `posChanged` |
| `currentPlayId` | `currentPlayIdChanged` |

---

## 4. 信号 (Signals)

| 信号签名 | 参数说明 | 备注 |
|----------|----------|------|
| `end()` | — | 播放结束 |
| `seq()` | — | 序列播放下一个 |
| `soundEnd()` | — | 单个声音播放结束 |
| `soundSuspend()` | — | 声音播放暂停 |
| `musicSuspend()` | — | 音乐播放暂停 |
| `soundPlayerStarting()` | — | 声音播放器启动中 |
| `posChanged(qint64 pos)` | 当前播放位置 | 属性通知信号 |
| `currentPlayIdChanged(QString currentPlayId)` | 当前播放 ID | 属性通知信号 |
| `closeAudioOutputState()` | — | 关闭音频输出状态变更 |
| `onDebugInfoReady()` | — | 调试信息就绪 |
| `onActionInfoReady()` | — | 动作信息就绪 |
| `onSoundProcessStateChanged(QProcess::ProcessState state)` | 进程状态 | 声音处理进程状态变化 |
| `onCloseAudioOutputState()` | — | 关闭音频输出状态（重载） |
| `onCloseAuduiOutput()` | — | 关闭音频输出回调 |

---

## 5. 公开槽 (Public Slots)

| 槽函数签名 | 返回值 | 说明 |
|-----------|--------|------|
| `play(QString qsWord, QString qsLang, QString qsPhonetic, int type)` | `void` | 播放文本（TTS），支持多语言和音标 |
| `stop()` | `void` | 停止播放 |
| `forceStop()` | `void` | 强制停止播放 |
| `playFile(QString qsFile)` | `void` | 播放指定音频文件 |
| `playFileData(QString)` | `void` | 播放内存中的音频数据 |
| `playMusic(QString, qint64 playbackRate)` | `void` | 播放音乐，支持变速 |
| `playMusicPiece(QString, qint64 beginPos, qint64 endPos)` | `void` | 播放音乐片段 |
| `resume()` | `void` | 恢复播放 |
| `suspend()` | `void` | 暂停播放 |
| `isPlaying()` | `bool` | 查询是否正在播放 |
| `initAudioDevice(bool bTDevice)` | `void` | 初始化音频输出设备 |
| `hasUkSound(QString) const` | `bool` | 检查是否拥有英式发音数据 |
| `hasUsSound(QString) const` | `bool` | 检查是否拥有美式发音数据 |
| `killSoundProcess()` | `void` | 杀死声音播放进程 |
| `killedByOtherProcess()` | `void` | 被其他进程终止 |
| `openAudioOutput()` | `void` | 打开音频输出 |
| `closeAudioOutput()` | `void` | 关闭音频输出 |
| `setupSharedMemroy()` | `void` | 设置共享内存（注：函数名拼写错误） |
| `soundStateMonitorThread()` | `void` | 声音状态监控线程（异步） |
| `outputChangedBtwHpAndSpk()` | `void` | 检测耳机与扬声器输出切换 |

---

## 6. 私有方法 (Private Methods)

通过 `__func__` 符号推断以下为私有/内部辅助函数：

| 函数名 | 说明 |
|--------|------|
| `~YSoundCenter()` | 析构函数 |
| `YSoundCenter(QObject *parent)` | 构造函数 |

---

## 7. 原始代码拼写错误

二进制符号名中保留了原始代码的笔误：

| 错误写法 | 正确应为 |
|----------|----------|
| `setupSharedMemroy` | `setupSharedMemory` |
| `onCloseAuduiOutput` | `onCloseAudioOutput` |

---

## 8. 设计概要

`YSoundCenter` 是系统中的**声音/音频核心管理类**，职责范围涵盖：

### 8.1 文本转语音 (TTS)
- `play()` 支持指定语种（`qsLang`）和音标（`qsPhonetic`）
- `hasUkSound()` / `hasUsSound()` 查询本地是否已部署对应语言数据包

### 8.2 音频文件播放
- `playFile()` 播放本地文件系统中的音频文件
- `playFileData()` 直接播放二进制数据（无需写入文件系统）

### 8.3 音乐播放
- `playMusic()` 支持 `playbackRate` 参数以实现变速播放
- `playMusicPiece()` 支持指定起止位置播放片段

### 8.4 输出设备管理
- `initAudioDevice()` 初始化音频硬件
- `openAudioOutput()` / `closeAudioOutput()` 打开/关闭音频输出
- `outputChangedBtwHpAndSpk()` 监听耳机（Headphone）与扬声器（Speaker）切换

### 8.5 进程生命周期管理
- 使用 `QProcess` 管理外部播放子进程
- `soundStateMonitorThread()` 在独立线程中监控子进程状态
- `onSoundProcessStateChanged()` 响应进程状态变化
- `killSoundProcess()` 强制终止子进程

### 8.6 共享内存通信
- `setupSharedMemroy()` 建立共享内存区域，用于与播放子进程进行 IPC 通信

### 8.7 播放状态跟踪
- 通过 `pos` / `duration` 属性跟踪播放进度
- `state` 属性以 `QProcess::ProcessState` 枚举反映当前运行状态
- 信号机制实现播放生命周期的事件通知（end / seq / soundEnd / suspend 等）

---

## 9. 元数据索引结构解析

元数据位于 `0xa0b4e0` 附近，该区域以 Intel-style 数组形式存储了信号/槽/属性在字符串区的偏移量。每个索引项结构为：

```
struct QtMetaDataEntry {
    uint32_t string_offset;   // 在字符串区中的偏移
    int32_t  type;            // 类型标识（0xffff = 信号/槽标记）
    uint32_t arg_count;       // 参数个数
    uint32_t unknown;         // 保留
};
```

共定义 17 个条目，对应 4 个属性 + 13 个信号/槽接口。

---

## 10. 与系统其他模块的关系

```
┌─────────────────────────────────────────────────────┐
│                    YSoundCenter                      │
│  ┌──────────┐  ┌───────────┐  ┌──────────────────┐  │
│  │  TTS引擎  │  │  音频设备  │  │  进程管理器(QProcess)│  │
│  └──────────┘  └───────────┘  └──────────────────┘  │
│       │               │                 │            │
│       ▼               ▼                 ▼            │
│  ┌──────────┐  ┌───────────┐  ┌──────────────────┐  │
│  │ 语言包文件│  │ ALSA/OSS  │  │ 播放器子进程(mpg321) │  │
│  └──────────┘  └───────────┘  └──────────────────┘  │
│                                                     │
│  ┌──────────────────────┐   ┌────────────────────┐  │
│  │   共享内存 (IPC)      │   │ 声音状态监控线程     │  │
│  └──────────────────────┘   └────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

---

## 11. 扫描时音乐中断：原生调用点与修复边界

以下证据来自仓库 `binary/YoudaoDictPen` 的 `nm -C` 与 AArch64 `objdump -d -C`，
不是根据播放界面状态推测，也不代表已完成真实扫描听感验证。

### 11.1 真正中断发生在 QML 返回之后

`YSystemBase::onScanStart()` 位于 `0x456118`：

```text
0x456204  bl YSystemBase::ocrStart()
          ... YWordBookManager::setCanAutoPlay(false) ...
0x456230  bl YSoundCenter::suspend()
0x456234  ... 更新扫描状态 ...
```

即使 `main.qml` / `YAudioPlayer.close()` 已经跳过扫描引起的暂停，原生函数仍在 QML
同步处理返回后暂停解码器。`YSoundCenter::suspend()`（`0x450a8c`）在 `0x450ae8`
向其 `+0x20` 的 `QProcess` 写入 `{"cmd":"suspend"}\n`（字符串位于 `0x954c1c`），
然后发出 `soundSuspend(g_playSeq)`。`YMediaPlayerManager::onSuspend(uint)`（`0x4f4118`）
在序列号匹配时设置 `PAUSED`。因此只拦通知、只保 ALSA 引用或稍后恢复，均不能保证扫描期间连续播放。

另一个明确不同的调用点是用户暂停：`YMediaPlayerManager::onClickedPause()`（`0x4f3d68`）
先更新状态，再在 `0x4f3d94` 调用 `suspend()`；这个调用必须原样保留。

### 11.2 PenMods 的精确拦截

`MusicPlayer.cpp` 的 `YSoundCenter::suspend` hook 仅在全部条件成立时跳过原函数：

- 返回地址恰好是已重定位的 `YSystemBase::onScanStart + 0x11c`；
- `onScanStart + 0x118` 的机器指令仍是指向已解析 `suspend` 符号的 AArch64 `BL`；
- 当前是 PenMods 接管的音乐，且 `fm.pause_on_scan=false`；
- 宿主音乐状态为 `PLAYING`，且音乐实体的播放序列号仍与解码器当前序列号相同。

验证相对符号和指令可适应 ASLR，但**不宣称适配未知固件布局**；不匹配时放行原函数。
不是按整个扫描时间窗屏蔽 `suspend/stop/forceStop`。显式暂停、跟读、Home、熄屏、录音以及
`pause_on_scan=true` 的主动暂停仍走原生行为。实际拦截会输出：

```text
[MusicPlayer] Prevented native scan-start suspend; decoder music continues
```

原先的 1.5 秒延迟恢复、扫描结束恢复、扫描前位置回写和额外 MUSIC 扫描引用已移除。
这样显式暂停不会被误判为厂商暂停后自动抢回，也不会倒退到扫描前位置。页面隐藏只保留原有
MUSIC 引用，窗口结束后若音乐已停止则释放；未配对 `ocrStop` 的路径仍有 15 秒窗口上限。
无需新增配置项，沿用 `fm.pause_on_scan=false` 的默认值。

### 11.3 发音和结果查询不是可安全全局忽略的暂停

同一原生二进制还有以下独立中断源：

| 原生路径 | 指令证据 |
|---|---|
| `YResultManager::doMainQuery` | `0x522820` 调用 `YSoundCenter::stop()` |
| `YResultManager::doKeyQuery` | `0x52300c` 调用 `stop()` |
| `doMainQueryES` / `doKeyQueryES` | `0x5238e8` / `0x523d68` 调用 `stop()` |
| `YSoundCenter::play(文本, 语言, 音标, int)` | `0x4516c0` 调用 `commonsStop()` |
| `YSoundCenter::playMusic(QString const&, qint64, double)` | `0x451dec` 调用 `commonsStop()` |

`commonsStop()` 在 `0x450bd8` 调用 `stop()`，随后递增 `g_playSeq`。
`currentPlayId()`（`0x450e68`）实际返回该 **uint32 序列号**，不是本报告早期元数据概要中的 QString。
`playMusic` 的完整原生签名也包含 `double` 速度参数。音乐 manager 的当前实体位于 `+0x20`，
其序列号位于实体 `+100`；`onSuspend` 自己就是按此字段区分当前音乐。

TTS 和音乐共用同一 `QProcess` 命令通道，发音入口主动结束旧播放。因此本修复保证的是
**不再发送扫描启动本身的 suspend 命令**，不是音乐与发音混音。扫描完成进入字典查询或实际
自动/手动发音仍可能中断音乐；不能把这些 `stop()` 全局吞掉，否则会破坏发音、序列号和状态同步。
这证明了软件通道共享，但不足以证明硬件不能混音；未进行硬件连续播放或双解码器测试。
结果页的音乐保留判断也检查解码器序列号，避免异步 `PLAYING` 状态尚未更新时误保护实际正在朗读的声音。

### 11.4 集成验证清单

本次修改不在工作代理中构建、部署或触发设备扫描；以下由集成验证执行：

1. `pause_on_scan=false`，已播放的本地音乐：扫描接触到 OCR 结束期间进度和声音持续，出现上述
   native-suspend 拦截日志，不出现定时重播/跳回扫描前位置。
2. 音乐本来已暂停时扫描：保持暂停且无拦截日志；扫描期间手动点暂停或耳机暂停：保持暂停，
   1.5 秒、OCR 结束和 15 秒之后均不自动恢复。
3. `pause_on_scan=true`：照常暂停，恢复和结果页位置保护不变。
4. 自动/手动朗读、跟读、录音、Home 和熄屏：原生音频优先级保留；尤其不能把发音误当作当前音乐。
5. 连续扫描及被拦截扫描：MUSIC 引用不随扫描次数增长，停止后可回到零；未知原生布局不匹配时不吞命令。

2026-11-14 真机集成烟测：通过 `FileManager::playFromView` 播放临时 WAV，调用真实
`YSystemBase::onScanStart()`（不是直接发信号）后，出现精确拦截日志，2 秒后宿主仍为
`PLAYING` 且 `currentPos` 增长；随后显式 `onClickedPause()` 保持暂停。此测试覆盖原生
扫描启动软件链路，不替代持笔连续扫描、自动发音及实际听感验证。
