// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "system/input/InputDaemon.h"
#include "system/input/HeadsetControls.h"
#include "system/input/ScreenManager.h"

#include "system/battery/BatteryInfo.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QByteArray>
#include <QFile>
#include <QSaveFile>
#include <QTimer>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mod {

namespace {

// daemon 的启动/停止都只在“状态不对”时等待，正常路径不 sleep，绝不在 UI 线程无界阻塞。
constexpr int         kDaemonStopTimeoutMs   = 200;
constexpr int         kDaemonStartTimeoutMs  = 150;
constexpr int         kDaemonStartAttempts   = 3;
constexpr int         kDaemonPollStepMs      = 20;
constexpr int         kDaemonDeferredRetryMs = 1500;
constexpr const char* kDaemonName            = "input-event-daemon";

// init 脚本用 `mount --bind <每机型文件> /etc/input-event-daemon.conf` 把机型配置挂到
// daemon 默认读取的位置（daemon 里硬编码的就是这个路径）。
constexpr const char* kEffectiveConfigPath = "/etc/input-event-daemon.conf";

std::string pidsOrNone(const std::string& pids) { return pids.empty() ? std::string("none") : pids; }

// /etc/input-event-daemon.conf 是 init 脚本 mount --bind 出来的文件挂载点。对挂载点做
// rename 一定 EBUSY，所以先判断挂载点、直接走原地镜像，避免每次重写都做一次注定失败的
// QSaveFile commit（也避免在 /etc 留下临时文件）。
// 注意：不能对 procfs 用 QFile::atEnd()（st_size==0 会立刻 EOF），必须 readAll + split。
bool isMountPoint(const std::string& path) {
    QFile file("/proc/self/mountinfo");
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray data = file.readAll();
    for (const QByteArray& line : data.split('\n')) {
        const auto fields = line.split(' ');
        if (fields.size() > 4 && fields[4] == QByteArray::fromStdString(path)) {
            return true;
        }
    }
    return false;
}

} // namespace

/*
 * Issue #22-1 诊断备注（YDP02X / 系统 2.1.2 / PenMods 2.5.5：充电过夜后“屏幕无法关闭且触控失效”）。
 *
 * 已证实（证据与反汇编见 build/diag/input-fix/）：
 *  - 充电时 vendor /usr/bin/system_sleep_wakeup 的 suspend 分支只对 battery status ==
 *    "Discharging" 真正 `echo mem > /sys/power/state`；插着线时它进入 while 循环每 60s
 *    轮询等拔出。input-event-daemon 的 {system_suspend} idle 动作会先 `killall -9 hilink-server`。
 *  - vendor 在“充电状态上升沿”（YBatteryManager::updateBatteryInfo）执行 `screen_onoff on`
 *    + `reset_input-event &`（后者会注入合成 touch press/release）+ 播 charging.mp3；
 *    满电时状态在 Full/Charging 间抖动就会反复走这个分支。
 *  - /usr/bin/screen_switch off/on 通过 /sys/kernel/debug/touchscreen/suspend 挂起/恢复触控：
 *    “触控失效”= 该节点停在 1 而屏幕已亮；vendor /usr/bin/runDictPen 的恢复兜底在 Exam 机型
 *    写的是不存在的 /sys/kernel/debug/touch3427/suspend。
 *  - 本模块旧实现每次 app 启动必然重启 daemon 两次、用截断方式写配置、killall 后不等旧进程
 *    退出、不校验启动结果；reset() 的排序守卫比较旧值，可能静默丢弃用户保存的休眠时长。
 *
 * 未证实假设（日志正是为了下次复现时定位它，本修复不依赖该假设）：
 *  vendor 充电沿的 screen_onoff on / 合成 touch 与 PenMods 的 daemon 重启、音频锁 pause/resume
 *  叠加后，daemon 的 freeze 状态、/tmp/screen_status、touch suspend 节点可能失步，形成
 *  “屏幕亮着 + 触控 suspend=1”。因此这里只在确实需要时重启一次 daemon、重启后重放 pause，
 *  并且 ScreenManager 在灭屏期间不再调用 resume()（SIGUSR2 会让积压输入触发 [Idle] reset）。
 */

InputDaemon::InputDaemon() : Logger("InputDaemon") {
    // task-9：耳机线控探测（默认关闭/仅检测到明确媒体键设备时启用，当前只识别不动作）。
    // 从这里实例化可以避免改动 task-9 写范围之外的 src/mod/Mod.cpp。
    HeadsetControls::getInstance();
    connect(&Event::getInstance(), &Event::uiCompleted, this, &InputDaemon::onUiCompleted);
}

void InputDaemon::onUiCompleted() {
    // uiCompleted 是 Qt 槽：这里任何异常都必须就地吞掉并记日志，否则会 std::terminate(SIGABRT)。
    try {
        // 上一进程若在音频锁生效时崩溃/被杀，会留下厂商的 pause 标记并把 daemon 留在 freeze
        // 状态（freeze 期间 daemon 不执行 [Idle] 的任何动作 => 自动灭屏/休眠全部失效）。
        // 厂商 cron 要等标记超过 20 分钟才会清理；本进程启动时明确是“未暂停”意图，这里主动
        // 清掉并记录，避免“屏幕/休眠一直不动作”的状态跨进程残留。
        if (QFile::exists("/tmp/stop_input_event_daemon")) {
            warn("启动时发现残留的 /tmp/stop_input_event_daemon（上一进程的 pause 未清理），主动恢复 auto screen off");
            resume("startup-clear-stale-pause");
        }
        info("UI 完成，按持久化设置应用 input-event-daemon 配置（reset）");
        reset();
    } catch (const std::exception& e) {
        error("InputDaemon::onUiCompleted 异常（已忽略）：{}", e.what());
    } catch (...) {
        error("InputDaemon::onUiCompleted 未知异常（已忽略）");
    }
}

bool InputDaemon::setScreenOff(uint32 sec) {
    mScreenOff     = sec;
    mBackLightDown = sec > 10 ? sec - 10 : 0;
    return _apply("setScreenOff");
}

bool InputDaemon::setSystemSuspend(uint32 sec) {
    // daemon 按时间顺序执行 idle 动作：suspend 的截止时间不晚于背光/灭屏，就会在
    // 屏幕还没关时先 suspend。这里只拦“运行期改动”，并在拒绝时明确打日志；持久化值
    // 由 reset() 原样下发，不会出现“用户设置被默认值悄悄替换”的情况。
    if (sec != 0 && mScreenOff != 0 && sec <= mBackLightDown) {
        warn(
            "[setSystemSuspend] 拒绝 {}s：必须晚于 backlight_down（{}s，screen_off={}s）",
            sec,
            mBackLightDown,
            mScreenOff
        );
        return false;
    }
    mSystemSuspend = sec;
    return _apply("setSystemSuspend");
}

void InputDaemon::setSystemSuspendOverride(bool enabled) {
    if (mSystemSuspendOverride == enabled) {
        return;
    }
    mSystemSuspendOverride = enabled;
    _apply(enabled ? "suspend-override-on" : "suspend-override-off");
}

void InputDaemon::reset() {
    // 持久化配置是权威来源：直接赋值，绕开运行期守卫。旧实现先 setScreenOff 再
    // setSystemSuspend，后者用旧的 mSystemSuspend 与新的 mBackLightDown 比较，既会
    // 静默丢弃用户保存的休眠时长，又让一次 reset 必然触发两次 killall/重启。
    mScreenOff     = ScreenManager::getInstance().getAutoSleepDuration();
    mBackLightDown = mScreenOff > 10 ? mScreenOff - 10 : 0;
    mSystemSuspend = BatteryInfo::getInstance().getAutoSuspendDuration();
    if (mSystemSuspend != 0 && mScreenOff != 0 && mSystemSuspend <= mBackLightDown) {
        warn(
            "持久化配置里 system_suspend={}s 不晚于 backlight_down={}s，仍按用户保存值写入",
            mSystemSuspend,
            mBackLightDown
        );
    }
    info(
        "[reset] 持久化设置：screen_off={}s（backlight_down={}s）system_suspend={}s",
        mScreenOff,
        mBackLightDown,
        mSystemSuspend
    );
    _apply("reset");
}

void InputDaemon::pause(const char* reason) {
    mPaused = true;
    try {
        const std::string pids = _daemonPids();
        info(
            "[{}] pause auto screen off：screen_on={} charging={} external_power={} daemon_pid={}",
            reason,
            mScreenOn,
            BatteryInfo::getInstance().isCharging(),
            BatteryInfo::getInstance().isOnExternalPower(),
            pidsOrNone(pids)
        );
        // 厂商实现 = `touch /tmp/stop_input_event_daemon` + `killall -USR1 input-event-daemon`
        // （daemon 进 freeze）。它只让 daemon 不再处理输入/idle，不会点亮屏幕。
        PEN_CALL(void*, "stop_auto_screen_off")();
    } catch (const std::exception& e) {
        error("[{}] pause auto screen off 异常（已忽略）：{}", reason, e.what());
    } catch (...) {
        error("[{}] pause auto screen off 未知异常（已忽略）", reason);
    }
}

void InputDaemon::resume(const char* reason) {
    mPaused = false;
    try {
        if (!mScreenOn) {
            // ScreenManager 已经用 mResumePending 拦了这类调用；这里再兜一层并留证据：
            // 灭屏时 SIGUSR2 会让 daemon 处理积压输入并执行 [Idle] reset（screen_onoff on），
            // 实测表现是“刚熄的屏幕又亮起来”。
            warn("[{}] resume auto screen off 被要求在灭屏状态下调用（延迟到屏幕亮起后再做）", reason);
        }
        const std::string pids = _daemonPids();
        info(
            "[{}] resume auto screen off：screen_on={} charging={} external_power={} daemon_pid={}",
            reason,
            mScreenOn,
            BatteryInfo::getInstance().isCharging(),
            BatteryInfo::getInstance().isOnExternalPower(),
            pidsOrNone(pids)
        );
        // 厂商实现 = `rm /tmp/stop_input_event_daemon` + `killall -USR2 input-event-daemon`。
        PEN_CALL(void*, "start_auto_screen_off")();
    } catch (const std::exception& e) {
        error("[{}] resume auto screen off 异常（已忽略）：{}", reason, e.what());
    } catch (...) {
        error("[{}] resume auto screen off 未知异常（已忽略）", reason);
    }
}

void InputDaemon::onScreenStateChanged(bool screenOn) {
    if (mScreenOn == screenOn) {
        return;
    }
    mScreenOn = screenOn;
    try {
        const std::string pids = _daemonPids();
        info(
            "屏幕{}：charging={} external_power={} paused={} daemon_pid={}",
            screenOn ? "亮起" : "熄灭",
            BatteryInfo::getInstance().isCharging(),
            BatteryInfo::getInstance().isOnExternalPower(),
            mPaused,
            pidsOrNone(pids)
        );
    } catch (const std::exception& e) {
        error("屏幕状态日志异常（已忽略）：{}", e.what());
    } catch (...) {
        error("屏幕状态日志未知异常（已忽略）");
    }
}

bool InputDaemon::isDaemonRunning() { return !_daemonPids().empty(); }

std::string InputDaemon::daemonPids() { return _daemonPids(); }

const InputDaemon::Config& InputDaemon::_config() {
    if (!mConfig.mLoaded) {
        mConfig         = _getConfig();
        mConfig.mLoaded = true;
        info("daemon 配置：model={} path={} effective={}", mConfig.mName, mConfig.mPath, mConfig.mEffectivePath);
    }
    return mConfig;
}

std::string InputDaemon::_renderConfig(const Config& cfg) const {
    // The audio lock suppresses suspend at runtime without discarding the
    // user's configured duration.
    const uint32 suspend = mSystemSuspendOverride ? 0 : mSystemSuspend;
    return QString::fromStdString(cfg.mContent)
        .replace("{backlight_down}", mBackLightDown ? QString::number(mBackLightDown) : "#")
        .replace("{screen_off}", mScreenOff ? QString::number(mScreenOff) : "#")
        .replace("{system_suspend}", suspend ? QString::number(suspend) : "#")
        .toStdString();
}

std::string InputDaemon::_currentConfigContent(const Config& cfg) const {
    const std::string effective = readFile(cfg.mEffectivePath.c_str());
    if (!effective.empty()) {
        return effective;
    }
    return readFile(cfg.mPath.c_str());
}

bool InputDaemon::_apply(const char* reason) {
    try {
        return _applyImpl(reason);
    } catch (const std::exception& e) {
        error("[{}] 应用 input-event-daemon 配置异常（已忽略本次）：{}", reason, e.what());
    } catch (...) {
        error("[{}] 应用 input-event-daemon 配置未知异常（已忽略本次）", reason);
    }
    return false;
}

bool InputDaemon::_applyImpl(const char* reason) {
    const Config&     cfg        = _config();
    const std::string rendered   = _renderConfig(cfg);
    const std::string pids       = _daemonPids();
    const bool        configSame = _currentConfigContent(cfg) == rendered;

    if (configSame && !pids.empty()) {
        // 配置没变就别重启：重启会清掉 daemon 的空闲状态并丢掉厂商的 freeze 状态
        // （实测下一次输入会走 [Idle] reset 把刚熄的屏幕点亮）。重复的相同调用降级到
        // debug，避免音频锁之类的频繁 toggle 刷屏。
        if (mSkipCount++ == 0 || mSkipContent != rendered) {
            info(
                "[{}] 配置无变化且 daemon 在跑（pid={}），跳过重启；screen_off={}s system_suspend={}s override={} "
                "charging={} screen_on={} paused={}",
                reason,
                pids,
                mScreenOff,
                mSystemSuspend,
                mSystemSuspendOverride,
                BatteryInfo::getInstance().isCharging(),
                mScreenOn,
                mPaused
            );
            mSkipContent = rendered;
        } else {
            debug("[{}] 配置仍无变化，跳过重启（第 {} 次）", reason, mSkipCount);
        }
        // 配置不用改，但 daemon 的 freeze 状态可能被外部（cron/init 重新拉起）清掉。
        // 只做“往暂停方向”的补偿：pause 不会点亮屏幕；反向的 resume 在灭屏过渡期是禁区，
        // 残留标记交给厂商 cron（20 分钟）或下次进程启动清理，这里只留证据。
        if (mPaused && !QFile::exists("/tmp/stop_input_event_daemon")) {
            warn("[{}] daemon 在跑但 pause 标记缺失，重放 pause 以恢复 freeze", reason);
            pause("resync-pause");
        } else if (!mPaused && QFile::exists("/tmp/stop_input_event_daemon")) {
            warn("[{}] 发现残留 /tmp/stop_input_event_daemon 但本进程未暂停；不在过渡期发 SIGUSR2", reason);
        }
        return true;
    }
    mSkipCount = 0;
    mSkipContent.clear();

    _logState("apply-begin", reason);
    if (!configSame) {
        info("[{}] daemon 配置与渲染结果不同 → 重写配置并重启 daemon", reason);
    } else {
        warn("[{}] 配置一致但 input-event-daemon 不在跑 → 重新拉起", reason);
    }

    // 先停 daemon：避免 bind mount 的 in-place 镜像有并发读者，也保证新 daemon 不会
    // 读到任何半写内容。（rename 是原子的；镜像旧 inode 那一步不是，原因见下。）
    _stopDaemon(reason);

    const std::string previous  = _currentConfigContent(cfg);
    const bool        installed = _installConfig(cfg, rendered, reason);
    if (!installed) {
        error("[{}] 配置写入失败，回滚到上一份内容后再拉起 daemon", reason);
        if (!previous.empty() && !_installConfig(cfg, previous, "rollback")) {
            error("[{}] 回滚也失败；daemon 将读取磁盘上的现有内容", reason);
        }
    }

    const bool started = _startDaemon(reason);
    if (started && mPaused) {
        // 新 daemon 从“未暂停”开始；厂商的 freeze 是运行期信号 + /tmp 标记，重启后就丢了。
        info("[{}] daemon 已重启，重放 pause（新进程默认未 freeze）", reason);
        pause("reapply-after-restart");
    }
    _logState(started ? "apply-end" : "apply-end(daemon-missing)", reason);
    if (!started) {
        // 有限重试都失败时不在 UI 线程死循环，交给事件循环稍后再试一次。
        QTimer::singleShot(kDaemonDeferredRetryMs, this, [this]() {
            if (!_daemonPids().empty()) {
                return;
            }
            warn("延迟重试拉起 input-event-daemon");
            if (_startDaemon("deferred-retry") && mPaused) {
                pause("reapply-after-deferred-retry");
            }
        });
    }
    return started;
}

bool InputDaemon::_installConfig(const Config& cfg, const std::string& rendered, const char* reason) {
    const QString effective    = QString::fromStdString(cfg.mEffectivePath);
    const bool    hasEffective = cfg.mEffectivePath != cfg.mPath && QFile::exists(effective);

    // daemon 实际读取 /etc/input-event-daemon.conf；boot 时 init 脚本把它 `mount --bind`
    // 到每机型文件上，两者共享同一个 inode（正常状态）。
    bool sharedInode      = false;
    bool effectiveMounted = false;
    if (hasEffective) {
        struct stat stModel{};
        struct stat stEffective{};
        if (::stat(cfg.mPath.c_str(), &stModel) == 0 && ::stat(cfg.mEffectivePath.c_str(), &stEffective) == 0) {
            sharedInode = stModel.st_dev == stEffective.st_dev && stModel.st_ino == stEffective.st_ino;
        }
        effectiveMounted = isMountPoint(cfg.mEffectivePath);
    }

    // 【A】正常状态：机型文件就是 bind mount 的源，两者共享 inode。
    // 只能通过机型文件（位于 rw 的 / 挂载上）原地写：写入的就是挂载点服务的那个 inode。
    // **不能 rename 机型文件**，也**不能直接写挂载点**：init 建立 bind mount 时 / 还是 ro，
    // 挂载点自身带 ro 标志（C2 实测对 /etc/input-event-daemon.conf 做 open(O_TRUNC) 返回
    // EROFS）；一旦 rename 换掉机型文件 inode，ro 挂载点仍固定旧 inode，之后再也更新不到
    // daemon 读的那份文件。所以这是本机型唯一无法"临时文件+rename"的路径：先停 daemon，
    // 再原地写+fsync，写完校验，且 daemon 只在启动时读配置 → 不会读到半写内容。
    if (sharedInode) {
        if (!_overwriteFileInPlace(cfg.mPath, rendered)) {
            error("[{}] bind mount 源文件原地写入失败：{}", reason, cfg.mPath);
            return false;
        }
        info(
            "[{}] {} 与 {} 共享 inode（bind mount 源）：通过可写源文件原地写入 {} 字节"
            "（挂载点只读、且不能 rename，见注释；daemon 已停止，无半写读者）",
            reason,
            cfg.mPath,
            cfg.mEffectivePath,
            rendered.size()
        );
        const std::string installed = _currentConfigContent(cfg);
        if (installed != rendered) {
            error(
                "[{}] 校验失败：{} 的内容与渲染结果不一致（读回 {} 字节，期望 {} 字节）",
                reason,
                cfg.mEffectivePath,
                installed.size(),
                rendered.size()
            );
            return false;
        }
        debug("[{}] 校验通过：{} 内容已是新配置", reason, cfg.mEffectivePath);
        return true;
    }

    // 【B】其它情况：机型文件可以原子替换。
    if (!_writeFileAtomic(cfg.mPath, rendered)) {
        warn("[{}] 原子写入 {} 失败，退化为原文件覆写", reason, cfg.mPath);
        if (!_overwriteFileInPlace(cfg.mPath, rendered)) {
            error("[{}] 无法写入 {}", reason, cfg.mPath);
            return false;
        }
    } else {
        info("[{}] 已原子写入配置 {}（{} 字节）", reason, cfg.mPath, rendered.size());
    }

    if (!hasEffective) {
        return true;
    }

    if (effectiveMounted) {
        // 分叉状态：机型文件曾被人 rename，ro 挂载点仍固定旧 inode（本代码的历史 bug 可能造成）。
        // 挂载点只读，无法就地修正；本次只更新机型文件，并明确告警 + 校验，重启后 init 重新
        // bind mount 即可自愈。
        warn(
            "[{}] {} 是只读 bind mount 且与机型文件 inode 已分离（历史 rename 遗留）；"
            "本次只更新 {}，daemon 在重启前仍读旧内容，重启后自愈",
            reason,
            cfg.mEffectivePath,
            cfg.mPath
        );
        if (readFile(cfg.mPath.c_str()) != rendered) {
            error("[{}] 校验失败：{} 内容与渲染结果不一致", reason, cfg.mPath);
            return false;
        }
        return true;
    }

    // 【C】非挂载点的独立文件：同样原子替换。
    if (_writeFileAtomic(cfg.mEffectivePath, rendered)) {
        info("[{}] 已原子替换 daemon 实际读取的 {}", reason, cfg.mEffectivePath);
    } else if (_overwriteFileInPlace(cfg.mEffectivePath, rendered)) {
        warn("[{}] 原子替换 {} 失败，已退化为原地覆写", reason, cfg.mEffectivePath);
    } else {
        error("[{}] 无法更新 {}", reason, cfg.mEffectivePath);
        return false;
    }

    if (_currentConfigContent(cfg) != rendered || readFile(cfg.mPath.c_str()) != rendered) {
        error("[{}] 校验失败：配置内容与渲染结果不一致", reason);
        return false;
    }
    debug("[{}] 校验通过：{} 与 {} 内容均为新配置", reason, cfg.mPath, cfg.mEffectivePath);
    return true;
}

bool InputDaemon::_stopDaemon(const char* reason) {
    const std::string before = _daemonPids();
    if (before.empty()) {
        debug("[{}] 没有运行中的 input-event-daemon", reason);
        return true;
    }
    info("[{}] 停止 input-event-daemon（pid={}）", reason, before);
    exec("killall input-event-daemon");
    // poll 而不是死等：killall 是异步的，不确认旧进程退出就启动新进程会出现两个
    // daemon 同时读同一批 event 设备（并让随后重启丢失的状态判断失准）。
    const bool gone = _waitForDaemonState(false, kDaemonStopTimeoutMs);
    if (!gone) {
        warn(
            "[{}] killall 后约 {}ms 仍能看到 input-event-daemon（pid={}），继续后续步骤",
            reason,
            kDaemonStopTimeoutMs,
            _daemonPids()
        );
    } else {
        info("[{}] input-event-daemon 已退出", reason);
    }
    return gone;
}

bool InputDaemon::_startDaemon(const char* reason) {
    // 绝不能给 exec() 加 -v/--verbose：daemon(1,1) 会保留 stdout（也就是 popen 的管道
    // 写端），exec() 里的 pclose() 就会一直等 EOF，把 UI 线程永久卡住。
    for (int attempt = 1; attempt <= kDaemonStartAttempts; ++attempt) {
        exec(kDaemonName);
        if (_waitForDaemonState(true, kDaemonStartTimeoutMs)) {
            const std::string pids = _daemonPids();
            info(
                "[{}] input-event-daemon 已启动（pid={}，第 {}/{} 次尝试）",
                reason,
                pids,
                attempt,
                kDaemonStartAttempts
            );
            return true;
        }
        warn(
            "[{}] 第 {}/{} 次拉起 input-event-daemon 后约 {}ms 内没看到进程",
            reason,
            attempt,
            kDaemonStartAttempts,
            kDaemonStartTimeoutMs
        );
    }
    error("[{}] {} 次尝试后仍无法拉起 input-event-daemon", reason, kDaemonStartAttempts);
    return false;
}

bool InputDaemon::_writeFileAtomic(const std::string& path, const std::string& content) {
    QSaveFile file(QString::fromStdString(path));
    // 不允许 QSaveFile 退化成“直接写目标文件”：那样截断瞬间就会暴露半写配置。
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        warn("QSaveFile 打开 {} 失败：{}", path, file.errorString().toStdString());
        return false;
    }
    const QByteArray data = QByteArray::fromStdString(content);
    if (file.write(data) != data.size()) {
        warn("QSaveFile 写入 {} 失败：{}", path, file.errorString().toStdString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        warn("QSaveFile 提交 {} 失败（bind mount 挂载点会返回 EBUSY）：{}", path, file.errorString().toStdString());
        return false;
    }
    return true;
}

bool InputDaemon::_overwriteFileInPlace(const std::string& path, const std::string& content) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        // EROFS 通常表示路径是 ro bind mount（本机型 /etc/input-event-daemon.conf），
        // 此时必须改走「写机型文件」的分支，详见 _installConfig 的注释。
        warn("open({}) 失败（原地覆写）：errno={} ({})", path, errno, std::strerror(errno));
        return false;
    }
    size_t written = 0;
    bool   ok      = true;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n <= 0) {
            ok = false;
            break;
        }
        written += static_cast<size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) {
        // fsync 失败不致命：紧接着启动的 daemon 读的是同一份 page cache。
        warn("fsync({}) 失败", path);
    }
    ::close(fd);
    if (!ok) {
        warn("写入 {} 失败（原地覆写，已写 {} 字节）", path, written);
    }
    return ok;
}

bool InputDaemon::_waitForDaemonState(bool running, int timeoutMs) {
    for (int waited = 0; waited <= timeoutMs; waited += kDaemonPollStepMs) {
        const bool isRunning = !_daemonPids().empty();
        if (isRunning == running) {
            return true;
        }
        ::usleep(kDaemonPollStepMs * 1000);
    }
    return !_daemonPids().empty() == running;
}

std::string InputDaemon::_daemonPids() {
    // exec() 在 popen 失败时会抛 std::runtime_error；这个值被日志/Qt 槽路径调用，不能外抛。
    try {
        return exec("pidof input-event-daemon");
    } catch (const std::exception& e) {
        warn("pidof input-event-daemon 执行失败：{}", e.what());
        return {};
    }
}

void InputDaemon::_logState(const char* phase, const char* reason) {
    const Config&     cfg      = _config();
    const std::string pids     = _daemonPids();
    const std::string rendered = _renderConfig(cfg);
    info(
        "[{}] phase={} screen_off={}s backlight_down={}s system_suspend={}s suspend_override={} "
        "charging={} external_power={} screen_on={} paused={} daemon_running={} daemon_pid={} "
        "config={} (model={}) content_matches={}",
        reason,
        phase,
        mScreenOff,
        mBackLightDown,
        mSystemSuspend,
        mSystemSuspendOverride,
        BatteryInfo::getInstance().isCharging(),
        BatteryInfo::getInstance().isOnExternalPower(),
        mScreenOn,
        mPaused,
        !pids.empty(),
        pidsOrNone(pids),
        cfg.mPath,
        cfg.mName,
        _currentConfigContent(cfg) == rendered
    );
}

InputDaemon::Config InputDaemon::_getConfig() {
    auto pcba = exec("get_pcba_version");
    if (pcba == "Dictpen2.0_V4") {
        return {"V4", "/etc/input-event-daemon_V4.conf", kEffectiveConfigPath, _getRawConfigure("V4")};
    }
    if (pcba == "Dictpen2.0_V0") {
        return {"V0", "/etc/input-event-daemon_V0.conf", kEffectiveConfigPath, _getRawConfigure("V0")};
    }
    if (pcba == "Exam_V0") {
        return {"Exam", "/etc/input-event-daemon_Exam.conf", kEffectiveConfigPath, _getRawConfigure("Exam")};
    }
    if (pcba.find("Cherry_V0") != std::string::npos) {
        return {"Cherry", "/etc/input-event-daemon_Cherry.conf", kEffectiveConfigPath, _getRawConfigure("Cherry")};
    }
    if (pcba.find("Cherry-3566") != std::string::npos || pcba == "Kiwi-3566_V0") {
        return {
            "Cherry_3566",
            "/etc/input-event-daemon_Cherry-3566.conf",
            kEffectiveConfigPath,
            _getRawConfigure("Cherry_3566")
        };
    }
    warn("Unable to find a matching input-event-daemon configuration file for this pcba({}).", pcba);
    return {"V0(fallback)", "/etc/input-event-daemon_V0.conf", kEffectiveConfigPath, _getRawConfigure("V0")};
}

std::string InputDaemon::_getRawConfigure(const char* model) {
    switch (H(model)) {
    case H("Cherry"):
        return R"(
#
# /etc/input-event-daemon.conf
#

[Global]
listen = /dev/input/by-path/platform-adc-keys-event
listen = /dev/input/by-path/platform-led_control-event
listen = /dev/input/by-path/platform-ff190000.i2c-event


[Keys]
#POWER        = system_sleep_wakeup auto
#POWER         = screen_onoff switch
#POWER        = system_sleep_wakeup resume
#MUTE         = amixer -q set Master mute
#FN+VOLUMEUP  = factory_reset_cfg
#CTRL+ALT+ESC = beep

[Switches]
RADIO:0 = ifconfig wlan0 down

[Idle]
{backlight_down}s = screen_onoff backlight_down
{screen_off}s = len_onoff.sh off; screen_onoff off
{system_suspend}s = killall -9 hilink-server; system_sleep_wakeup suspend
reset = sleep 0.1;screen_onoff on; sleep 1; killall -9 system_sleep_wakeup
)";
    case H("Cherry_3566"):
        return R"(#
# /etc/input-event-daemon.conf
#

[Global]
listen = /dev/input/by-path/platform-adc-keys-event
listen = /dev/input/by-path/platform-led_control-event
listen = /dev/input/by-path/platform-fe5a0000.i2c-event

[Keys]
#POWER        = system_sleep_wakeup auto
#POWER         = screen_onoff switch
#POWER        = system_sleep_wakeup resume
#MUTE         = amixer -q set Master mute
#FN+VOLUMEUP  = factory_reset_cfg
#CTRL+ALT+ESC = beep

[Switches]
RADIO:0 = ifconfig wlan0 down

[Idle]
{backlight_down}s = screen_onoff backlight_down
{screen_off}s = len_onoff.sh off; screen_onoff off
{system_suspend}s = killall -9 hilink-server; system_sleep_wakeup suspend
reset = sleep 0.1; screen_onoff on; system_post_wakeup; sleep 1; killall -9 system_sleep_wakeup
)";
    case H("V4"):
        return R"(
#
# /etc/input-event-daemon.conf
#

[Global]
listen = /dev/input/event2
listen = /dev/input/event3
listen = /dev/input/event4

[Keys]
#POWER        = system_sleep_wakeup auto
#POWER         = screen_onoff switch
#POWER        = system_sleep_wakeup resume
#MUTE         = amixer -q set Master mute
#FN+VOLUMEUP  = factory_reset_cfg
#CTRL+ALT+ESC = beep

[Switches]
RADIO:0 = ifconfig wlan0 down

[Idle]
{backlight_down}s = screen_onoff backlight_down
{screen_off}s = len_onoff.sh off; screen_onoff off
{system_suspend}s = killall -9 hilink-server; system_sleep_wakeup suspend
reset = screen_onoff on; sleep 1; killall -9 system_sleep_wakeup
)";
    case H("Exam"):
        return R"(
#
# /etc/input-event-daemon.conf
#

[Global]
listen = /dev/input/event2
listen = /dev/input/event3
listen = /dev/input/event4

[Keys]
#POWER        = system_sleep_wakeup auto
#POWER         = screen_onoff switch
#POWER        = system_sleep_wakeup resume
#MUTE         = amixer -q set Master mute
#FN+VOLUMEUP  = factory_reset_cfg
#CTRL+ALT+ESC = beep

[Switches]
RADIO:0 = ifconfig wlan0 down

[Idle]
{backlight_down}s = screen_onoff backlight_down
{screen_off}s = len_onoff.sh off; screen_onoff off
{system_suspend}s = killall -9 hilink-server; system_sleep_wakeup suspend
reset = sleep 0.1; screen_onoff on; system_post_wakeup; sleep 1; killall -9 system_sleep_wakeup
)";
    case H("V0"):
    default:
        return R"(#
# /etc/input-event-daemon.conf
#

[Global]
listen = /dev/input/event1
listen = /dev/input/event2
listen = /dev/input/event3

[Keys]
#POWER        = system_sleep_wakeup auto
#POWER         = screen_onoff switch
#POWER        = system_sleep_wakeup resume
#MUTE         = amixer -q set Master mute
#FN+VOLUMEUP  = factory_reset_cfg
#CTRL+ALT+ESC = beep

[Switches]
RADIO:0 = ifconfig wlan0 down

[Idle]
{backlight_down}s = screen_onoff backlight_down
{screen_off}s = len_onoff.sh off; screen_onoff off
{system_suspend}s = system_sleep_wakeup suspend
reset = screen_onoff on; sleep 1; killall -9 system_sleep_wakeup
)";
    }
}

} // namespace mod
