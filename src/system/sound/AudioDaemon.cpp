// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "system/sound/AudioDaemon.h"

#include "base/Hook.h"
#include "base/YPointer.h"
#include "common/Event.h"
#include "common/Utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <signal.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>

namespace mod {

static const char* sourceToString(AudioSource source) {
    switch (source) {
    case AudioSource::TTS:       return "TTS";
    case AudioSource::MUSIC:     return "MUSIC";
    case AudioSource::FILE_PLAY: return "FILE_PLAY";
    case AudioSource::SYSTEM:    return "SYSTEM";
    }
    return "UNKNOWN";
}

static const char* stateToString(AudioDaemonState state) {
    switch (state) {
    case AudioDaemonState::IDLE:        return "IDLE";
    case AudioDaemonState::PLAYING:     return "PLAYING";
    case AudioDaemonState::CLOSE_DELAY: return "CLOSE_DELAY";
    }
    return "UNKNOWN";
}

AudioDaemon::AudioDaemon() : Logger("AudioDaemon") {
    info("AudioDaemon 正在初始化...");

    // 延迟关闭定时器（单次触发）
    mCloseDelayTimer = new QTimer(this);
    mCloseDelayTimer->setSingleShot(true);
    connect(mCloseDelayTimer, &QTimer::timeout, this, &AudioDaemon::_onCloseTimeout);

    // 状态重评估定时器（每 15 秒触发一次）
    // 用于检测外部进程死后未触发 inotify 事件的孤儿锁场景
    mCleanupTimer = new QTimer(this);
    connect(mCleanupTimer, &QTimer::timeout, this, &AudioDaemon::_onCleanupTick);

    // inotify 防抖定时器（单次触发，50ms）
    mInotifyDebounceTimer = new QTimer(this);
    mInotifyDebounceTimer->setSingleShot(true);
    connect(mInotifyDebounceTimer, &QTimer::timeout, this, [this]() { _onWakeLockChange(); });

    // 读取配置（如果存在）
    json cfg = Config::getInstance().read("AudioDaemon");
    if (cfg.contains("enabled"))        mEnabled      = cfg["enabled"].get<bool>();
    if (cfg.contains("closeDelayMs"))   mCloseDelayMs = cfg["closeDelayMs"].get<int>();
    if (cfg.contains("wakelock_dir"))   mWakeLockDir  = QString::fromStdString(cfg["wakelock_dir"].get<std::string>());

    // 连接 UI 初始化完成事件
    connect(&Event::getInstance(), &Event::uiCompleted, this, [this]() {
        // 确保锁目录存在
        QDir().mkpath(mWakeLockDir);
        // 初始化 inotify 监控锁目录
        _initInotify();
        // 启动状态重评估定时器
        mCleanupTimer->start(15000);
        info("AudioDaemon 就绪。closeDelayMs={}, wakelock_dir={}", mCloseDelayMs, mWakeLockDir.toStdString());
    });
}

// ========== 引用计数接口 ==========

int AudioDaemon::acquire(AudioSource source) {
    if (!mEnabled) {
        info("acquire({}) 忽略（daemon 未启用）", sourceToString(source));
        return mRefCount;
    }

    bool needsOpen      = (mRefCount == 0 && mState == AudioDaemonState::IDLE);
    int& sourceRefCount = mSourceRefCounts[_sourceIndex(source)];

    // 每个 source 的首个引用持有对应锁文件，避免重复 acquire 被一次 release 提前解锁。
    if (sourceRefCount == 0) {
        QDir().mkpath(mWakeLockDir);
        QString lockFile = _lockFilePath(source);
        QFile   f(lockFile);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            warn("acquire({}) 无法创建 lockFile={}", sourceToString(source), lockFile.toStdString());
        } else {
            f.write(QByteArray::number(static_cast<long long>(getpid())));
        }
    }

    sourceRefCount++;
    mRefCount++;
    info("acquire({}) → refCount={}, sourceRefCount={}", sourceToString(source), mRefCount, sourceRefCount);
    emit refCountChanged(mRefCount);

    // 如果音频输出当前处于关闭状态（IDLE），主动打开
    if (needsOpen) {
        info("acquire: 引用计数从 0 增加，主动打开音频输出");
        auto result = PEN_CALL(uint64, "_ZN12YSoundCenter15openAudioOutputEv", void*)(YPointer<YSoundCenter>::getInstance());
        info("acquire: openAudioOutput 主动打开完成, ret={}", result);
    }

    return mRefCount;
}

int AudioDaemon::release(AudioSource source) {
    if (!mEnabled) {
        info("release({}) 忽略（daemon 未启用）", sourceToString(source));
        return mRefCount;
    }

    int& sourceRefCount = mSourceRefCounts[_sourceIndex(source)];
    if (sourceRefCount <= 0) {
        warn("release({}) 调用时该 source 的引用已为 0！", sourceToString(source));
        return mRefCount;
    }

    sourceRefCount--;
    mRefCount--;
    if (sourceRefCount == 0) {
        QFile::remove(_lockFilePath(source));
    }

    info("release({}) → refCount={}, sourceRefCount={}", sourceToString(source), mRefCount, sourceRefCount);
    emit refCountChanged(mRefCount);
    return mRefCount;
}

// ========== 决策接口（供 detour 调用） ==========

bool AudioDaemon::shouldOpenAudioOutput() {
    if (!mEnabled) return true;  // 不启用时，直接放行

    if (mPendingForceOpen) {
        mPendingForceOpen = false;
        info("强制执行 openAudioOutput（外部唤醒锁输出刷新）");
        return true;
    }

    switch (mState) {
    case AudioDaemonState::IDLE:
        // 需要打开
        _transitionTo(AudioDaemonState::PLAYING);
        return true;

    case AudioDaemonState::PLAYING:
        // 已经在播放中，无需重复打开
        info("抑制 openAudioOutput（已在 PLAYING 状态）");
        return false;

    case AudioDaemonState::CLOSE_DELAY:
        // 正在等待关闭，取消延迟定时器，回到 PLAYING
        mCloseDelayTimer->stop();
        info("取消延迟关闭，恢复为 PLAYING");
        _transitionTo(AudioDaemonState::PLAYING);
        return false;  // 音频设备应该还开着，无需再 open
    }
    return true;
}

void AudioDaemon::notifyOpenDone() {
    mTotalOpenCalls++;
    info("openAudioOutput 完成（累计={}）", mTotalOpenCalls);
    // 确保状态正确
    if (mState != AudioDaemonState::PLAYING) {
        _transitionTo(AudioDaemonState::PLAYING);
    }
}

bool AudioDaemon::shouldCloseAudioOutput() {
    if (!mEnabled) return true;

    // 检查是否有待处理的强制关闭请求（延迟超时后需要真正关闭）
    if (mPendingForceClose) {
        mPendingForceClose = false;
        info("强制执行 closeAudioOutput（延迟关闭超时后放行）");
        return true;
    }

    switch (mState) {
    case AudioDaemonState::IDLE:
        // 已经是空闲，无需关闭
        info("抑制 closeAudioOutput（已在 IDLE 状态）");
        return false;

    case AudioDaemonState::PLAYING:
        // 判断是否有任何唤醒锁（refCount + 文件锁）
        if (mRefCount == 0) {
            int fileLocks = _countWakeLocks();
            if (fileLocks == 0) {
                // 没有任何引用，进入延迟关闭
                _transitionTo(AudioDaemonState::CLOSE_DELAY);
                return false;
            }
            // 有外部文件锁占用，无需关闭
            info("抑制 closeAudioOutput（refCount=0, fileLocks={}）", fileLocks);
        } else {
            info("抑制 closeAudioOutput（refCount={}）", mRefCount);
        }
        return false;

    case AudioDaemonState::CLOSE_DELAY:
        // 已经在延迟关闭中，由定时器决定
        return false;
    }
    return true;
}

void AudioDaemon::notifyCloseDone() {
    mTotalCloseCalls++;
    info("closeAudioOutput 完成（累计={}）", mTotalCloseCalls);
    _transitionTo(AudioDaemonState::IDLE);
}

// ========== 状态机 ==========

void AudioDaemon::_transitionTo(AudioDaemonState newState) {
    if (mState == newState) return;

    info("状态: {} → {}", stateToString(mState), stateToString(newState));
    mState = newState;
    emit stateChanged(mState);

    switch (newState) {
    case AudioDaemonState::IDLE:
        mCloseDelayTimer->stop();
        break;

    case AudioDaemonState::PLAYING:
        break;

    case AudioDaemonState::CLOSE_DELAY:
        mCloseDelayTimer->start(mCloseDelayMs);
        break;
    }
}

// ========== 文件唤醒锁操作 ==========

QString AudioDaemon::_lockFilePath(AudioSource source) const {
    return QString("%1/%2.lock").arg(mWakeLockDir, sourceToString(source));
}

int AudioDaemon::_countWakeLocks() {
    QDir dir(mWakeLockDir);
    if (!dir.exists()) return 0;

    QStringList filters;
    filters << "*.lock";
    auto entries = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    int  count   = 0;

    for (const auto& entry : entries) {
        // 读取锁文件中的 PID，检查进程是否存活
        QFile f(entry.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        QByteArray pidData = f.readAll().trimmed();
        f.close();

        bool  ok   = false;
        pid_t pid  = pidData.toInt(&ok);
        if (ok && kill(pid, 0) == 0) {
            // 进程存活，这是一个有效的锁
            count++;
        } else {
            if (pidData.isEmpty()) {
                // 没有 PID 数据，可能文件刚创建，视为有效锁
                count++;
            } else {
                // 有 PID 但进程已死 → 孤儿锁，顺手删除
                info("清理孤儿锁文件: {} (pid={} 已死)", entry.absoluteFilePath().toStdString(), pid);
                QFile::remove(entry.absoluteFilePath());
            }
        }
    }
    return count;
}

void AudioDaemon::_initInotify() {
    mInotifyFd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (mInotifyFd < 0) {
        error("inotify_init1 失败: {}", strerror(errno));
        return;
    }

    auto dirPath = mWakeLockDir.toStdString();
    mInotifyWd = inotify_add_watch(mInotifyFd,
                                    dirPath.c_str(),
                                    IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVED_TO);

    if (mInotifyWd < 0) {
        error("inotify_add_watch 失败: {}", strerror(errno));
        close(mInotifyFd);
        mInotifyFd = -1;
        return;
    }

    // 通过 QSocketNotifier 将 inotify fd 集成到 Qt 事件循环
    mInotifyNotifier = new QSocketNotifier(mInotifyFd, QSocketNotifier::Read, this);
    connect(mInotifyNotifier, &QSocketNotifier::activated, this, &AudioDaemon::_onInotifyReady);
}

void AudioDaemon::_onInotifyReady(int fd) {
    Q_UNUSED(fd);

    if (mInotifyFd < 0) {
        return;
    }

    // 使用固定大小缓冲区循环读取（避免 VLA 栈溢出）
    // sizeof(struct inotify_event) + NAME_MAX + 1 是单个事件的最大尺寸
    // 乘以 64 足够一次性处理任意批量事件
    union {
        char     raw[64 * (sizeof(struct inotify_event) + NAME_MAX + 1)];
        uint64_t align;
    } buf;
    constexpr size_t kBufSize = sizeof(buf.raw);

    auto n = read(mInotifyFd, buf.raw, kBufSize);
    if (n > 0) {
        // 扫描事件，检查是否有 IN_DELETE_SELF（监控目录被删）
        bool dirDeleted = false;
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            auto* event = reinterpret_cast<struct inotify_event*>(buf.raw + offset);
            if (event->mask & IN_DELETE_SELF) {
                dirDeleted = true;
            }
            offset += sizeof(struct inotify_event) + event->len;
        }

        if (dirDeleted) {
            // 监控目录被删，重新创建并添加 watch
            info("inotify 监控目录被删除，重新创建并添加 watch");
            QDir().mkpath(mWakeLockDir);
            if (mInotifyWd >= 0) {
                inotify_rm_watch(mInotifyFd, mInotifyWd);
            }
            auto dirPath = mWakeLockDir.toStdString();
            mInotifyWd = inotify_add_watch(mInotifyFd,
                                            dirPath.c_str(),
                                            IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVED_TO);
            if (mInotifyWd < 0) {
                error("重新添加 inotify watch 失败: {}", strerror(errno));
            }
        }

        // 防抖：50ms 内的连续事件合并为一次状态检查
        _scheduleWakeLockCheck();
    } else if (n < 0 && errno != EAGAIN) {
        error("inotify read 错误: {}", strerror(errno));
        // 严重的 inotify 错误，重建整个 inotify 实例
        if (mInotifyNotifier) {
            delete mInotifyNotifier;
            mInotifyNotifier = nullptr;
        }
        if (mInotifyFd >= 0) {
            close(mInotifyFd);
            mInotifyFd  = -1;
            mInotifyWd  = -1;
        }
        info("重新初始化 inotify");
        _initInotify();
    }
    // n == 0 或 EAGAIN：无事可做，忽略
}

void AudioDaemon::_onWakeLockChange(bool refreshExternalOutput) {
    int fileLocks = _countWakeLocks();
    info("wake lock 目录变更: fileLocks={}, refCount={}, state={}",
          fileLocks, mRefCount, stateToString(mState));

    if (fileLocks > 0 || mRefCount > 0) {
        // 有新的唤醒锁创建（外部进程 touch 了锁文件），需要确保音频输出已打开
        if (mState == AudioDaemonState::IDLE) {
            info("检测到新的唤醒锁，主动打开音频输出");
            auto result = PEN_CALL(uint64, "_ZN12YSoundCenter15openAudioOutputEv", void*)(YPointer<YSoundCenter>::getInstance());
            info("_onWakeLockDirChanged: openAudioOutput 主动打开完成, ret={}", result);
            return;
        }

        if (mState == AudioDaemonState::CLOSE_DELAY) {
            // 取消延迟关闭，回到 PLAYING
            mCloseDelayTimer->stop();
            _transitionTo(AudioDaemonState::PLAYING);
        }

        // 底层输出可能在静默期间自行休眠；定期扫描到外部锁时强制重新打开。
        if (refreshExternalOutput && fileLocks > 0 && mRefCount == 0) {
            mPendingForceOpen = true;
            auto result = PEN_CALL(uint64, "_ZN12YSoundCenter15openAudioOutputEv", void*)(YPointer<YSoundCenter>::getInstance());
            info("外部唤醒锁输出刷新完成, ret={}", result);
        }
    } else if (mState == AudioDaemonState::PLAYING && mRefCount == 0) {
        // 所有文件锁已释放且 refCount 也为 0，进入延迟关闭
        info("所有唤醒锁已释放，进入延迟关闭");
        _transitionTo(AudioDaemonState::CLOSE_DELAY);
    }
}

// ========== 定时器回调 ==========

void AudioDaemon::_scheduleWakeLockCheck() {
    mInotifyDebounceTimer->start(50);
}

void AudioDaemon::_onCleanupTick() {
    // 定期重评估会清理孤儿锁，并为仍有效的外部锁刷新底层音频输出。
    _onWakeLockChange(true);
}

void AudioDaemon::_onCloseTimeout() {
    int fileLocks = _countWakeLocks();
    if (mState != AudioDaemonState::CLOSE_DELAY || mRefCount > 0 || fileLocks > 0) {
        info("延迟关闭超时取消（state={}, refCount={}, fileLocks={}）", stateToString(mState), mRefCount, fileLocks);
        if (mState == AudioDaemonState::CLOSE_DELAY) {
            _transitionTo(AudioDaemonState::PLAYING);
        }
        return;
    }

    info("延迟关闭超时已到期，执行 closeAudioOutput");
    // 先设置强制关闭标记，使得 PEN_HOOK detour 放行
    mPendingForceClose = true;
    // 直接通过 PEN_CALL 调用原始 closeAudioOutput（会经过 PEN_HOOK，但 mPendingForceClose 让其放行）
    // notifyCloseDone 由 hook 负责调用
    auto result = PEN_CALL(uint64, "_ZN12YSoundCenter16closeAudioOutputEv", void*)(YPointer<YSoundCenter>::getInstance());
    info("closeAudioOutput 直接关闭完成, ret={}", result);
}

} // namespace mod

// ========== PEN_HOOK 定义 ==========

// Hook: openAudioOutput — 所有系统打开音频输出的请求都经由此处
PEN_HOOK(uint64, _ZN12YSoundCenter15openAudioOutputEv, void* self) {
    auto& daemon = mod::AudioDaemon::getInstance();

    // 询问 AudioDaemon 是否应该调用原始函数
    if (daemon.shouldOpenAudioOutput()) {
        auto result = origin(self);
        daemon.notifyOpenDone();
        return result;
    }

    // 被抑制，返回 0（表示成功，但未执行实际操作）
    return 0;
}

// Hook: closeAudioOutput — 所有系统关闭音频输出的请求都经由此处
PEN_HOOK(uint64, _ZN12YSoundCenter16closeAudioOutputEv, void* self) {
    auto& daemon = mod::AudioDaemon::getInstance();

    // 询问 AudioDaemon 是否应该调用原始函数
    if (daemon.shouldCloseAudioOutput()) {
        auto result = origin(self);
        daemon.notifyCloseDone();
        return result;
    }

    // 被抑制，返回 0
    return 0;
}