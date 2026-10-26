// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

namespace mod {

class InputDaemon : public QObject, public Singleton<InputDaemon>, private Logger {
    Q_OBJECT

public:
    void onUiCompleted();

    // Set to 0 will never execute;
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
    // 注意：两者的实现都是 `_console_run()` 起的 shell 命令，本身不会点亮屏幕，但
    // SIGUSR2 会让 daemon 处理 freeze 期间积压的输入事件，进而可能执行 [Idle] reset
    // （screen_onoff on）——所以灭屏/唤醒过渡期间不要随手调用 resume()。
    void pause(const char* reason = "unspecified");

    void resume(const char* reason = "unspecified");

    // 由 ScreenManager 通知屏幕状态。这里刻意不调用 daemon，也不用它决定是否重启：
    // 过渡期间给厂商脚本/daemon 发信号正是会点亮屏幕的那类调用。
    void onScreenStateChanged(bool screenOn);

    bool isScreenOn() const { return mScreenOn; }

    // 只读诊断接口（日志与设备排查用）。非 const：内部 exec() 失败时要走 Logger 的 warn，
    // 而 spdlog 的日志方法是 non-const 的。
    bool        isDaemonRunning();
    std::string daemonPids();

private:
    friend Singleton<InputDaemon>;
    explicit InputDaemon();

    struct Config {
        std::string mName;          // pcba 型号键（Exam/V4/V0/Cherry...）
        std::string mPath;          // 每机型配置文件；boot 时被 init 脚本 bind mount
        std::string mEffectivePath; // daemon 实际读取的 /etc/input-event-daemon.conf
        std::string mContent;       // 未替换占位符的模板
        bool        mLoaded = false;
    };

    uint32 mBackLightDown = 30;
    uint32 mScreenOff     = 60;
    uint32 mSystemSuspend = 600;

    bool mSystemSuspendOverride = false;

    // 运行期状态（仅用于日志与“重启后重放 pause”）。
    bool mPaused   = false;
    bool mScreenOn = true;

    // 相同配置的重复 apply 只在第一次记 info，其余降到 debug，避免刷屏。
    int         mSkipCount = 0;
    std::string mSkipContent;

    Config mConfig;

    // 单次渲染 + 原子写 + 必要时（且只重启一次）重启 daemon。返回 daemon 是否在跑。
    // 外层负责把所有 std::exception 吞掉并记日志：这些路径由 Qt 槽/厂商 hook 调用，
    // 异常逃出去会 std::terminate（合并版曾因未防护的 nlohmann 访问在 uiCompleted 崩溃）。
    bool _apply(const char* reason);
    bool _applyImpl(const char* reason);

    const Config& _config();

    std::string _renderConfig(const Config& cfg) const;

    // daemon 真正读到的内容：bind mount 的 /etc/input-event-daemon.conf（若存在），
    // 否则退回每机型配置文件。判断“配置有没有变”必须以它为准。
    std::string _currentConfigContent(const Config& cfg) const;

    bool _installConfig(const Config& cfg, const std::string& rendered, const char* reason);

    bool _stopDaemon(const char* reason);

    bool _startDaemon(const char* reason);

    bool _writeFileAtomic(const std::string& path, const std::string& content);

    bool _overwriteFileInPlace(const std::string& path, const std::string& content);

    bool _waitForDaemonState(bool running, int timeoutMs);

    std::string _daemonPids();

    void _logState(const char* phase, const char* reason);

    Config _getConfig();

    std::string _getRawConfigure(const char* model);
};

} // namespace mod
