# touch-injector

YDP02X 上通过 `/dev/uinput` 注入触摸/指针事件的调试工具，用来在没有任何自动化接口的
QML 界面上做验证（AGENTS.md 的「Touch injection」一节描述的就是这套做法）。

内核是 4.4，没有 `UI_DEV_SETUP` / `UI_ABS_SETUP`，所以走 legacy `uinput_user_dev`：

- 设备必须是**绝对坐标指针**：`EV_ABS` 的 `ABS_X`(0..170) / `ABS_Y`(0..320) + `BTN_LEFT`，
  且**不要**设 `INPUT_PROP_DIRECT`。MT-B 触摸事件会被 Weston 吃掉，Qt 客户端收不到。
- 坐标映射：UI 逻辑点 `(ux, uy)`（ux∈[0,319], uy∈[0,169]）→ `raw_x = uy`、`raw_y = 319 - ux`；
  左手模式（应用自己把内容旋转 180°）加 `--lefthand`，即先换成 `(319-ux, 169-uy)`。

## 构建与使用

```sh
zig cc -target aarch64-linux-gnu.2.27 -O2 -o injector injector.c    # glibc 目标不能 -static
adb push injector /userdisk/ && adb shell 'chmod +x /userdisk/injector'

I=/userdisk/injector
$I tap   ui 160 107                # 点击
$I longtap ui 160 107              # 600ms 长按
$I hold  ui 160 107 3000           # 按住 3s（可在这期间截图，比如看长按倍速徽标）
$I swipe ui 300 107 20 107         # 横向拖动（滚列表 / 拖动跳转）
$I flicktap ui 160 150 160 60 160 90 60   # 先甩动再点：60ms 后在滑行中的列表上点击
$I tap   ui 20 15 --lefthand       # 左手模式坐标
```

`flicktap` 存在的理由：每次调用都要重新创建 uinput 设备并等 Weston 打开（300ms），分两次调用时
「甩动后的惯性滑行」早就结束了，所以必须在同一次设备会话里完成甩动 + 延时 + 点击。
`<delay_ms>` 是松手到按下之间的等待，用来落在惯性滑行窗口内（默认 `flickDeceleration` 下，一次
90px/288ms 的甩动滑行约 200ms，所以 60ms 仍落在 `flicking` 里，300ms 已经结束）。实测结论见
AGENTS.md「Touch injection」一节。

`/userdisk/` 是持久分区（`/tmp` 是 tmpfs，每次重启都会丢），所以放在这里重启后还能用；
调试完记得删掉。每次注入的坐标都要用截图确认（`weston-screenshooter` 需要 Weston 带 `--debug`）。
