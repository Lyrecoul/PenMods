// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "base/YEnum.h"

#include "common/service/Logger.h"

#include <QDir>
#include <QTimer>

namespace mod::filemanager {

using PlayFile = std::shared_ptr<QFileInfo>;
using PlayList = std::vector<PlayFile>;

class MusicPlayer : public QObject, public Singleton<MusicPlayer>, private Logger {
    Q_OBJECT

    Q_PROPERTY(bool pauseOnScan READ getPauseOnScan WRITE setPauseOnScan NOTIFY pauseOnScanChanged);
    Q_PROPERTY(bool hideFloatingWindow READ getHideFloatingWindow WRITE setHideFloatingWindow NOTIFY hideFloatingWindowChanged);
    // 下拉面板上次停留的视图: true = 音乐控制, false = 快捷设置
    Q_PROPERTY(bool quickPanelMusicView READ getQuickPanelMusicView WRITE setQuickPanelMusicView NOTIFY quickPanelMusicViewChanged);
    Q_PROPERTY(bool shutdownTimerEnabled READ shutdownTimerEnabled NOTIFY shutdownTimerChanged);
    Q_PROPERTY(int shutdownTimerMinutes READ shutdownTimerMinutes NOTIFY shutdownTimerChanged);
    Q_PROPERTY(bool shutdownAfterPlaylist READ shutdownAfterPlaylist WRITE setShutdownAfterPlaylist NOTIFY shutdownTimerChanged);

public:
    /// 播放列表中的第 idx 首。
    /// @param showPlayerUi 是否把宿主播放页（YGlobal::showAudioPlayer）调到前台：只有用户显式
    ///        选曲才为 true；曲末自动切曲/单曲循环必须为 false，否则每首放完都会弹回播放页。
    void play(size_t idx, bool showPlayerUi = true);

    void clickNext(bool showPlayerUi = true);

    void clickPrev(bool showPlayerUi = true);

    void clickRand(bool showPlayerUi = true);

    void onSoundEnd();

    PlayList& getPlayListRef() { return mPlayList; };

    static AudioSequence getCurrentAudioSequence();

    static bool mIsTakeOver;

    [[nodiscard]] bool isPausedByScan() const { return mPausedByScan; }
    int64_t            normalizePositionDuringScan(int64_t requestedPosition);
    void                  onPlaybackResumedAfterScan();
    Q_INVOKABLE bool      isScanPauseActive() const { return mPausedByScan; }
    Q_INVOKABLE bool      shouldPreserveMusicOnScanResultClose();
    Q_INVOKABLE void      finishScanPause();

    [[nodiscard]] bool getPauseOnScan() const;
    void setPauseOnScan(bool enabled);

    [[nodiscard]] bool getHideFloatingWindow() const;
    void setHideFloatingWindow(bool hidden);

    [[nodiscard]] bool getQuickPanelMusicView() const;
    void setQuickPanelMusicView(bool showMusicControls);

    /// 供 QML 调用：将当前音频定位到指定的毫秒位置
    Q_INVOKABLE void seekToPosition(qint64 position);

    /// 供 QML 调用：终止当前音乐并释放播放器状态
    Q_INVOKABLE void stop();

    /// 供 QML 调用：释放当前 MUSIC 引用（播放停止/关闭播放器时）
    Q_INVOKABLE void releaseAudio();

    /// 页面导航隐藏播放器时，保留由扫描暂停的播放会话
    Q_INVOKABLE void releaseAudioAfterHide();
    Q_INVOKABLE void setShutdownTimerMinutes(int minutes);
    Q_INVOKABLE void cancelShutdownTimer();
    bool shutdownTimerEnabled() const { return mShutdownTimer.isActive() || mShutdownPending; }
    int shutdownTimerMinutes() const { return mShutdownTimerMinutes; }
    bool shutdownAfterPlaylist() const { return mShutdownAfterPlaylist; }
    void setShutdownAfterPlaylist(bool enabled);

    /// 清理当前临时软链接
    void cleanupTempSymlinks();

signals:
    void pauseOnScanChanged();
    void hideFloatingWindowChanged();
    void quickPanelMusicViewChanged();
    void shutdownTimerChanged();
    void stopRequested();

private:
    friend Singleton<MusicPlayer>;
    explicit MusicPlayer();

    PlayList mPlayList;

    struct {
        PlayFile mFile;
        size_t   mIndex{0};
        bool     mIsEnd{true};

        void setPlaying(size_t idx) {
            mIsEnd = false;
            mIndex = idx;
        }

    } mCurrentPlaying;

    /// 为指定文件在 /tmp 创建 .mp3 后缀的临时软链接
    /// @return 返回 .mp3 软链接路径，若原文件已是 .mp3 则返回原路径
    QString createTempSymlinks(const PlayFile& file, QString& outLrcPath);

    void _play(const PlayFile& file, bool showPlayerUi);

    // 临时软链接路径，用于清理
    QString mTempAudioLink;
    bool    mPauseOnScan{false};
    bool    mHideFloatingWindow{false};
    bool    mQuickPanelMusicView{false};
    QTimer  mShutdownTimer;
    int     mShutdownTimerMinutes{0};
    bool    mShutdownPending{false};
    bool    mShutdownAfterPlaylist{false};
    bool    mPausedByScan{false};
    bool    mPlaybackResumedDuringScan{false};
    bool    mScanResultClosed{false};
    int64_t mScanPausePosition{0};

    /// 开关关闭时的"扫描播放保护"：厂商 QML 的扫描流程会关播放器并暂停（main.qml onOcrStart →
    /// closeAudioPlayer()），且释放 MUSIC 引用会让 AudioDaemon 关掉 ALSA 设备；窗口内只救 PAUSED 那一次。
    bool    mScanInProgress{false};
    bool    mResumeAfterScan{false};
    bool    mScanLockHeld{false};
    int64_t mScanResumePosition{0};
    QTimer  mScanGuardTimer;

    void onOcrStarted();
    void onOcrStopped();
    /// 宿主播放器此刻是否在播（扫描窗口结束时据此决定要不要补一次释放）。
    bool isHostPlaying() const;
    void resumeAfterScan();
    void acquireScanLock();
    void releaseScanLock();
    void restoreScanPausePosition();
};
} // namespace mod::filemanager
