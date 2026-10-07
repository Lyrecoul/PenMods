# mem-profile

词典笔（460 MB 内存）上给宿主进程拍内存画像的小工具，用来回答"某功能开关后内存有没有正常还回来"。
目前主要服务内嵌 FFmpeg 播放器（`doc/FFMPEG_PLAYER_ANALYSIS.md`），但脚本与具体功能无关。

```sh
# 本地/直连设备
./mem-profile.sh

# 从开发机通过 adb
./mem-profile.sh -d 2D90500000818624

# watch 模式：每 2 秒一行，最多 5 行 —— 用手/browser 或 tools/touch-injector 驱动界面时抓峰值
./mem-profile.sh -d 2D90500000818624 -w 2 -n 5
```

输出示例：

```
pid=5800 RSS=213512kB threads=44 dma_buf=7(10.9MB) module_mapped=12452kB rw_seg=267 anon_rw_seg=116
```

| 字段 | 含义 |
|---|---|
| `RSS` | `/proc/<pid>/status` 的 VmRSS |
| `threads` | 线程数 —— 播放器关闭后必须回到基线，否则是解码/音频线程没 join |
| `dma_buf=N(xMB)` | `/sys/kernel/debug/dma_buf/bufinfo` 里的对象数与总大小。硬解时 MPP 帧池会多出十几个，**关闭后必须回到基线**（这部分是 DRM/CMA 内存，泄漏比匿名页更致命） |
| `module_mapped` | `-m`（默认 `ffmpegplayer`）匹配到的映射总大小 —— QML 插件 .so 一旦被引擎加载就**不会卸载**，所以这个值只应在第一次打开后固定住 |
| `rw_seg` / `anon_rw_seg` | `rw-p` 段数与其中的匿名段数，用来看堆有没有随次数增长 |

## 判读基线（2026-10-07 实测，YDP02X）

一次干净的开关（重启宿主 → 进文件管理器 → 打开视频 → 播放 → 返回）：

| 状态 | RSS | threads | dma-buf | module_mapped |
|---|---|---|---|---|
| 刚启动 | 178.5 MB | 44 | 7 | 0 |
| 进到文件管理器（没开播放器） | 195.3 MB | 44 | 7 | 0 |
| 播放中（硬解 640×360 + 零拷贝） | 228.2 MB | 50 | **26** | 12.45 MB |
| 关闭后 / 再等 10 s | 213.4 / 213.5 MB | **44** | **7** | 12.45 MB |

结论（细节见 `doc/FFMPEG_PLAYER_ANALYSIS.md` §11）：

- **正常释放**：线程回到 44、dma-buf 回到 7、ALSA 关闭、`/tmp/audio_wakelocks/` 锁文件删除、
  播放期间多出的 4~6 MB 匿名页回收。
- **回收不掉**：插件 .so 的 12.45 MB 映射（QML 插件机制决定）+ 一次性库/GL/QML 初始化，
  合计首次打开后留下约 18 MB；此外每次开关还有一点分配器/QML GC 滞留。
- **不是泄漏**：连续开关 9 次的"关闭后"RSS 增量依次为
  16.3 / 2.6 / 1.6 / 2.4 / 1.0 / 1.6 / 0.14 / 0.16 / 0.22 MB，逐次衰减并收敛到 ~0.2 MB，
  之后平台期稳定在约 221.7 MB；`rw_seg`/`anon_rw_seg` 也不随次数增长。

判断新改动是否引入泄漏的做法：跑 6~9 次开关，看"关闭后 RSS"的增量是否持续不降、
`threads` 是否高于基线、`dma_buf` 是否高于基线。三者任一持续增长才值得深挖。

## 相关

- `tools/touch-injector/` —— 在没有任何自动化接口的 QML 界面上点按/滑动，配合 watch 模式抓峰值。
- 截图需要 Weston 带 `--debug`（`weston-screenshooter`），见 AGENTS.md 的「Screenshots」一节。
