// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"
#include "mod/Config.h"

#include <QSocketNotifier>
#include <QTimer>

#include <array>

namespace mod {

/// 音频源类型，用于引用计数跟踪
enum class AudioSource {
    TTS,        ///< TTS 播放（扫描翻译朗读）
    MUSIC,      ///< 音乐播放（MusicPlayer）
    FILE_PLAY,  ///< 文件音频播放
    SYSTEM,     ///< 系统音频（录音等）
};

/// 音频守护进程运行状态
enum class AudioDaemonState {
    IDLE,           ///< 无音频输出活动
    PLAYING,        ///< 音频输出已激活（引用计数 > 0）
    CLOSE_DELAY,    ///< 所有引用已释放，等待延迟关闭超时
};

/**
 * @brief 音频输出生命周期守护进程
 *
 * 拦截 YSoundCenter::openAudioOutput() 和 closeAudioOutput()，
 * 通过引用计数 + 文件唤醒锁 + 延迟关闭 + 状态机智能管理音频设备生命周期。
 *
 * 内部模块（MusicPlayer、AudioRecorder）使用 acquire()/release() API。
 * 外部进程通过创建/删除 /tmp/audio_wakelocks/ 下的文件来获取/释放唤醒锁。
 *
 * 文件系统监控使用 inotify（比 QFileSystemWatcher 更可靠）。
 */
class AudioDaemon : public QObject, public Singleton<AudioDaemon>, private Logger {
    Q_OBJECT
    Q_PROPERTY(int refCount READ refCount NOTIFY refCountChanged)
    Q_PROPERTY(AudioDaemonState daemonState READ state NOTIFY stateChanged)

public:
    /// 获取音频输出引用。返回新的引用计数。
    int acquire(AudioSource source);

    /// 释放音频输出引用。返回新的引用计数。
    int release(AudioSource source);

    /// 当前所有音频源的引用总数
    int refCount() const { return mRefCount; }

    /// 指定音频源的引用数
    int sourceRefCount(AudioSource source) const { return mSourceRefCounts[_sourceIndex(source)]; }

    /// 当前守护进程状态
    AudioDaemonState state() const { return mState; }

    /// 启用/禁用守护进程
    void setEnabled(bool enabled) { mEnabled = enabled; }
    bool isEnabled() const { return mEnabled; }

    // --- hook 决策辅助接口 ---
    bool shouldOpenAudioOutput();
    void notifyOpenDone();
    bool shouldCloseAudioOutput();
    void notifyCloseDone();

signals:
    void refCountChanged(int newRefCount);
    void stateChanged(AudioDaemonState newState);

private:
    friend Singleton<AudioDaemon>;
    friend struct std::default_delete<AudioDaemon>;
    explicit AudioDaemon();

    // --- 状态机 ---
    static constexpr size_t _sourceIndex(AudioSource source) { return static_cast<size_t>(source); }
    void _transitionTo(AudioDaemonState newState);

    // --- 唤醒锁文件操作 ---
    QString _lockFilePath(AudioSource source) const;
    int     _countWakeLocks();
    void    _onWakeLockChange(bool refreshExternalOutput = false);

    // --- inotify ---
    void _initInotify();
    void _onInotifyReady(int fd);

    // --- 定时器回调 ---
    void _onCloseTimeout();
    void _onCleanupTick();
    void _scheduleWakeLockCheck();

    // --- 状态 ---
    AudioDaemonState mState{AudioDaemonState::IDLE};
    int              mRefCount{0};
    std::array<int, 4> mSourceRefCounts{};
    bool             mEnabled{true};
    bool             mPendingForceOpen{false};
    bool             mPendingForceClose{false};

    // --- 配置 ---
    int     mCloseDelayMs{3000};
    QString mWakeLockDir{"/tmp/audio_wakelocks"};

    // --- 定时器 ---
    QTimer* mCloseDelayTimer{nullptr};
    QTimer* mCleanupTimer{nullptr};
    QTimer* mInotifyDebounceTimer{nullptr};

    // --- inotify ---
    int                   mInotifyFd{-1};
    int                   mInotifyWd{-1};
    QSocketNotifier*      mInotifyNotifier{nullptr};

    // --- 统计 ---
    uint64 mTotalOpenCalls{0};
    uint64 mTotalCloseCalls{0};
};

} // namespace mod