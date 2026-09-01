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

public:
    void play(size_t idx);

    void clickNext();

    void clickPrev();

    void clickRand();

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

    /// 供 QML 调用：将当前音频定位到指定的毫秒位置
    Q_INVOKABLE void seekToPosition(qint64 position);

    /// 供 QML 调⽤：释放当前 MUSIC 引⽤（播放停⽌/关闭播放器时）
    Q_INVOKABLE void releaseAudio();

    /// 页面导航隐藏播放器时，保留由扫描暂停的播放会话
    Q_INVOKABLE void releaseAudioAfterHide();

    /// 清理当前临时软链接
    void cleanupTempSymlinks();

signals:
    void pauseOnScanChanged();

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

    void _play(const PlayFile& file);

    // 临时软链接路径，用于清理
    QString mTempAudioLink;
    bool    mPauseOnScan{false};
    bool    mPausedByScan{false};
    bool    mPlaybackResumedDuringScan{false};
    bool    mScanResultClosed{false};
    int64_t mScanPausePosition{0};

    void onOcrStarted();
    void restoreScanPausePosition();
};
} // namespace mod::filemanager
