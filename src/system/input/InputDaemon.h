// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include <cstdint>
#include <memory>

namespace mod {

class InputDaemon : public QObject, public Singleton<InputDaemon>, private Logger {
    Q_OBJECT

public:
    ~InputDaemon() override;
    void onUiCompleted();

    // Set to 0 will never execute. Returns whether the request was accepted;
    // configuration installation and process control complete asynchronously.
    bool setScreenOff(uint32 sec);
    bool setSystemSuspend(uint32 sec);

    // Runtime-only override: while enabled the {system_suspend} idle action is
    // disabled, without touching the user's persisted value, so the configured
    // duration is restored as soon as the override is cleared. Used by the audio
    // lock to keep a playing device from suspending (which would drop Bluetooth).
    void setSystemSuspendOverride(bool enabled);

    // Reset input daemon from cfg.
    void reset();

    // temporary settings.
    //
    // 对应厂商 librkdev 的 stop/start_auto_screen_off：pause 创建
    // /tmp/stop_input_event_daemon 并向 daemon 发送 SIGUSR1（daemon 进入 freeze，期间
    // 既不读输入事件也不执行 idle 动作），resume 删除标记并发送 SIGUSR2。
    //
    // 工作线程通过标记文件和信号实现同样的协议，避免厂商 shell 包装阻塞 UI。但
    // SIGUSR2 会让 daemon 处理 freeze 期间积压的输入事件，进而可能执行 [Idle] reset
    // （screen_onoff on）——所以灭屏/唤醒过渡期间不要随手调用 resume()。
    void pause(const char* reason = "unspecified");

    void resume(const char* reason = "unspecified");

    // Publish screen state immediately; the worker rechecks it before SIGUSR2.
    void onScreenStateChanged(bool screenOn);

    bool isScreenOn() const { return mScreenOn; }

    bool isUpdating() const;

    // Cached process discovery from the worker; never invokes a shell on the GUI.
    bool        isDaemonRunning();
    std::string daemonPids();

private:
    friend Singleton<InputDaemon>;
    explicit InputDaemon();

    class Backend;
    std::unique_ptr<Backend> mBackend;
    void _scheduleUpdate(const char* reason);
    bool mUpdateQueued = false;
    bool mConfigure = false;
    uint64_t mConfigRevision = 0;
    bool mControlPause = false;
    std::string mReason;

    uint32 mBackLightDown = 30;
    uint32 mScreenOff     = 60;
    uint32 mSystemSuspend = 600;

    bool mSystemSuspendOverride = false;

    // 运行期状态（仅用于日志与“重启后重放 pause”）。
    bool mPaused   = false;
    bool mScreenOn = true;
};

} // namespace mod
