# 远程截屏通道（ScreenGrabber）

> 目标: 不依赖任何外部工具，从电脑侧拿到设备当前屏幕的一张 PNG
>
> 适用: 调试与真机验收；任何页面都行（词典主界面 / 设置 / 锁屏 / 插件页）

---

## 1. 用法

```bash
ADB=adb                       # 或 adb -s <序列号>

# 1) 请求抓一帧：只要一个空文件
$ADB shell touch /tmp/penmods_shot

# 2) 1 秒内出结果，看日志
$ADB shell cat /tmp/penmods_shot.log

# 3) 取走（序号自增，一次开机内从 0 开始）
$ADB pull /tmp/penmods_shot-0.png
```

日志每抓一次追加一行，成功形如：

```
ok /tmp/penmods_shot-0.png 320x170 exposed=0
```

## 2. 为什么不用现成的办法

`AGENTS.md` 里的 `weston-screenshooter` 是首选，但它要求 weston 带 `--debug`。本机 cmdline 只有
`weston --tty=2 -B=drm-backend.so --idle-time=0`，实测直接拒：

```
weston_screenshooter@5: error 0: screenshooter failed: permission denied. Debug protocol must be enabled
```

`AGENTS.md` 给的绕法是改 `/etc/init.d/S50launcher` 加 `--debug` 再重启 —— 要动 `/` 分区，代价太大。
其余外部途径也都不通：

| 办法 | 结果 |
| --- | --- |
| 外部 wayland 客户端 / wlr-screencopy | 厂商 weston 只有 fullscreen-shell 类 shell，建不出窗口 |
| `qmlscene --platform offscreen/vnc` + `grabToImage` | 无 GL ⇒ `item is not attached to a window` |
| 读 `/dev/fb0` | **禁止**：mmap 实测触发内核 panic |

宿主自己的 `QQuickView` 就是那个窗口，`QQuickWindow::grabWindow()` 走的是场景图 FBO，与上面这些外部途径无关。
附带好处：拿到的是 Qt 逻辑分辨率（320×170），不必像 compositor 截屏那样把 170×320 的原生帧再转 270°。

## 3. 实现要点

- 通道 = 哨兵文件 + 1 秒轮询（每轮两次 `stat()`）；不需要端口、常驻进程或唤醒锁。
- `beforeUiInitialization` 拿到的 view 是常驻的 ⇒ 任何页面都能截，不限于插件页前台。
- Timer **必须在 `uiCompleted` 之后才 start**：单例在 `__attribute__((constructor))` 阶段构造，
  那时事件循环还不存在，提前 start 会静默失效（症状：日志有「宿主窗口已就绪」，
  但哨兵永远不被消费、日志文件一行不写）。
- `grabWindow()` 必须在 GUI 线程调用，否则写 `err wrong-thread`。
- 日志里的 `exposed=` 只做记录、不当门槛：本机抓图成功时 `isExposed()` 仍为 `false`
  （Qt 在这个平台收不到 exposure 事件），拿它判断「能不能截」会永远截不到。

## 4. 失败原因对照

抓不到图时先看 `/tmp/penmods_shot.log`，比猜快：

| 日志 | 含义 |
| --- | --- |
| `err no-view` | 宿主窗口还没建（`beforeUiInitialization` 未到），稍后重试 |
| `err wrong-thread` | 不在 GUI 线程调用 |
| `err grab-null` | `grabWindow()` 返回空图 |
| `err no-png-enc fmt=...` | 设备没编 PNG 编码器，`fmt=` 后是实际支持的格式 |
| `err save-fail ...` | 写文件失败（`/tmp` 满或权限） |

`no-png-enc` 与 `save-fail` 都可能留下 **0 字节** PNG —— 别把「文件存在」当成抓成功。
