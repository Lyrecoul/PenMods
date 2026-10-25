// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

#include <QObject>
#include <QQuickView>
#include <QTimer>

namespace mod {

/// 远程截屏通道（调试 / 验收用）。
///
/// 用途：从电脑侧拿到设备**当前屏幕**的一帧，任何页面都行（词典主界面 / 设置 /
/// 锁屏 / 插件页），不需要额外进程、端口或唤醒锁。
///
/// 为什么不用 weston-screenshooter：它要求 weston 带 --debug。本机 cmdline 只有
/// `weston --tty=2 -B=drm-backend.so --idle-time=0`，实测直接拒：
///   screenshooter failed: permission denied. Debug protocol must be enabled
/// AGENTS.md 给的绕法是改 /etc/init.d/S50launcher 加 --debug 再重启 —— 要动 / 分区，
/// 代价太大。其余外部途径也都不通：外部 wayland 客户端建不出窗口（厂商 weston 只有
/// fullscreen-shell 类 shell）；qmlscene 的 offscreen / vnc 后端没有 GL ⇒ grabToImage
/// 报 item is not attached to a window；/dev/fb0 不能碰（mmap 实测内核 panic）。
///
/// 而宿主自己的 QQuickView 就是那个窗口，QQuickWindow::grabWindow() 走的是场景图 FBO，
/// 和上面这些外部途径不是一回事。beforeUiInitialization 拿到的 view 是常驻的，所以
/// 任何页面都能截。附带好处：拿到的是 Qt 逻辑分辨率（320x170），不必像 compositor
/// 截屏那样把 170x320 的原生帧再转 270°。
///
/// 通道设计（哨兵文件）：
///   电脑侧 `touch /tmp/penmods_shot`
///     ⇒ 1 秒内抓一帧，存 /tmp/penmods_shot-<n>.png，结果追加进 /tmp/penmods_shot.log
///   电脑侧 `adb pull` 取走。
///
/// 不用 QML 的 grabToImage：那个只在插件页在前台、且 item 已挂到窗口时才有效。
///
/// 代价：1 秒一次、两次 stat()，不持唤醒锁。
class ScreenGrabber : public QObject, public Singleton<ScreenGrabber>, private Logger {
    Q_OBJECT

public:
    /// 立刻抓一帧，返回 PNG 路径；失败返回错误标识（同时写进 /tmp/penmods_shot.log）。
    QString grabNow();

private:
    friend Singleton<ScreenGrabber>;
    explicit ScreenGrabber();

    void tick();

    // Timer 用指针 + this 作 parent，生命周期跟着单例走，省掉析构。
    //
    // ⚠️ 真正容易踩的是 **start() 的时机**：本单例在 __attribute__((constructor))
    // 阶段（Mod::load）构造，那时 QCoreApplication 还没建、事件循环更没转起来，
    // 在这里 start() 只是把定时器排进一个尚不存在的循环，之后也不会自己补上。
    // 症状很有迷惑性：日志有「宿主窗口已就绪」，但哨兵永远不被消费、log 一行不写。
    // 所以 start() 放到 uiCompleted（Event 由 PEN_HOOK 在 GUI 线程里 emit）——
    // ScreenManager 把工作挪到 GUI 线程靠的也是这条。
    QTimer*     mTimer = nullptr;
    QQuickView* mView  = nullptr;
    int         mSeq   = 0;
    int         mTick  = 0;
    bool        mBusy  = false;
};

} // namespace mod
