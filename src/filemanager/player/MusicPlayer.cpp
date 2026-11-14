// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "filemanager/player/MusicPlayer.h"
#include "filemanager/player/LyricClock.h"
#include "filemanager/player/LyricLineItem.h"

#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "common/util/System.h"
#include "mod/Config.h"

#include "media/MediaSession.h"

#include "system/input/ScreenManager.h"
#include "system/sound/AudioDaemon.h"

#include <QCryptographicHash>
#include <QFile>
#include <QQmlContext>
#include <QRandomGenerator>

#define PLAYER_FAKE_COLUMN_ID ("fake_column_hsxjsbw")

namespace mod::filemanager {

/// 读宿主当前媒体的 localFile（YMediaPlayerManager 的 mCurrentAudio 在 +0x20，YMediaEntity 从 +0x10 开始）。
/// 宿主自己也是这么取的：onClickedPlay 里 `ldr x0,[x20,#32]` + `add x0,x0,#0x10` 后调 YMediaEntity::localFile。
static QString hostLocalFile(void* manager) {
    if (!manager) return {};
    const auto holder = *(uintptr_t*)((uintptr_t)manager + 0x20);
    if (!holder) return {};
    return PEN_CALL(QString, "_ZNK12YMediaEntity9localFileEv", void*)((void*)(holder + 0x10));
}

// The manager's PLAYING notification is queued: TTS may already own the decoder while the UI still
// says PLAYING. Never protect that pronunciation using a stale music state.
static bool hostOwnsDecoder(void* soundCenter) {
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    if (!manager || !soundCenter) return false;
    const auto holder = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(manager) + 0x20);
    if (!holder) return false;
    const auto musicSequence = *reinterpret_cast<uint32_t*>(holder + 100);
    return musicSequence != 0
        && musicSequence == PEN_CALL(uint32_t, "_ZNK12YSoundCenter13currentPlayIdEv", void*)(soundCenter);
}

/**
 * 临时 `.mp3` 软链接的根目录。
 *
 * 宿主（YMediaPlayerManager / YSoundCenter）按后缀决定走哪条解码路径，非 mp3 文件必须先变成
 * 一个 `.mp3` 路径。链接放在模块目录而不是 `/tmp`：`/tmp` 是 tmpfs，会被重启和其它进程清掉，
 * 而链接一旦从 `localFile` 上消失，宿主就把这条媒体当成已删除的文件（`fileNotExist()` +
 * `onResetPlayer()`），之后再按播放键永远没反应 —— 就是“暂停后无法再次播放”。
 */
static QString tempAudioLinkRoot() {
    const auto moduleDir = util::getStateDirectory();
    if (!moduleDir.isEmpty()) {
        return moduleDir + "/audiolinks";
    }
    return QStringLiteral("/tmp/penmods-audiolinks");
}

bool MusicPlayer::mIsTakeOver{false};

MusicPlayer::MusicPlayer() : Logger("MusicPlayer") {
    const auto cfg       = Config::getInstance().read("fm");
    mPauseOnScan         = cfg.value("pause_on_scan", false);
    mHideFloatingWindow  = cfg.value("hide_floating_window", false);
    mQuickPanelMusicView = cfg.value("quick_panel_music_view", false);

    connect(&Event::getInstance(), &Event::ocrStarted, this, &MusicPlayer::onOcrStarted);
    connect(&Event::getInstance(), &Event::ocrStopped, this, &MusicPlayer::onOcrStopped);
    // 兜底：连扫/点查/被 mod 拦下的扫描不一定都发 ocrStop，扫描窗口必须有硬上限（见 onOcrStarted）。
    mScanGuardTimer.setSingleShot(true);
    connect(&mScanGuardTimer, &QTimer::timeout, this, &MusicPlayer::onOcrStopped);
    mShutdownTimer.setSingleShot(true);
    connect(&mShutdownTimer, &QTimer::timeout, this, [this]() {
        // 倒计时结束：定时关机；"播放结束关机" 模式不使用该定时器（见 setShutdownAfterPlaylist）。
        cancelShutdownTimer();
        ScreenManager::getInstance().requestPowerOff();
    });
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        qmlRegisterType<LyricClock>("com.github.penuniverse", 1, 0, "LyricClock");
        qmlRegisterType<LyricLineItem>("com.github.penuniverse", 1, 0, "LyricLineItem");
        context->setContextProperty("musicPlayer", this);
    });
    // 上个进程留下的链接不会被任何会话引用，直接清掉，避免按歌曲数无限累积。
    _purgeStaleAudioLinks();
}

enum class LrcState {
    BILINGUAL,
    ORIGINAL,
    TRANS,
    HIDE,
};

enum class DownloadState { NOT, SUCCEED, ING, FAILURE, CANCEL, PAUSE };

struct YMediaEntity {
    YMediaEntity() = delete;
    char          unk[10];
    QString       mMediaId;         // 0x10
    QString       mOwnerId;         // 0x18
    QString       mTitle;           // 0x20
    int           mDuration;        // 0x28
    DownloadState mDownloadState;   // 0x2C
    QString       mUrl;             // 0x30
    QString       mLocalFile;       // 0x38
    QString       mLrcFile;         // 0x40
    LrcState      mLrcState;        // 0x48
    bool          mSrcAudioVisible; // 0x4C
};

struct YColumnMediaEntity : public YMediaEntity {
    int     mId;       // 0x50, repeat of mMediaId;
    QString mColumnId; // 0x58, repeat of mOwnerId;
    int     mProgress; // 0x60
    bool    mIsDir;    // 0x64
};

static_assert(sizeof(YColumnMediaEntity) == 0x68);

void MusicPlayer::play(size_t idx, bool showPlayerUi) {
    if (idx > mPlayList.size() - 1) return;
    auto file = mPlayList.at(idx);
    if (!mCurrentPlaying.mIsEnd && mCurrentPlaying.mFile == file && isHostPlaying()) {
        // 同一首还在播：这次调用只是"把播放页调出来"，不重新起播。
        if (showPlayerUi) {
            PEN_CALL(void*, "_ZN7YGlobal15showAudioPlayerEv", void*)(YPointer<YGlobal>::getInstance());
        }
        return;
    }
    debug("play idx={} showPlayerUi={}", idx, showPlayerUi);
    mCurrentPlaying.setPlaying(idx, file);
    _play(file, showPlayerUi);
}

void MusicPlayer::_play(const std::shared_ptr<QFileInfo>& file, bool showPlayerUi) {
    cancelOnlineLyrics();
    mIsTakeOver                = true;
    mPausedByScan              = false;
    mPlaybackResumedDuringScan = false;
    mScanResultClosed          = false;
    // 播放新曲目前先释放之前的 MUSIC 引用，防止该 source 的引用累积。
    if (mod::AudioDaemon::getInstance().sourceRefCount(AudioSource::MUSIC) > 0) {
        mod::AudioDaemon::getInstance().release(AudioSource::MUSIC);
    }
    // 通知音频守护进程：音乐播放器正在使用音频输出
    mod::AudioDaemon::getInstance().acquire(AudioSource::MUSIC);
    PEN_CALL(void, "_ZN19YMediaPlayerManager8wipeDataEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    PEN_CALL(bool, "_ZN7YGlobal23setAudioPlayingColomnIdERK7QString", void*, QString const&)
    (YPointer<YGlobal>::getInstance(), "myimport");
    auto memory = new char[sizeof(YColumnMediaEntity)];
    PEN_CALL(void, "_ZN18YColumnMediaEntityC2EP7QObject", void*, void*)(memory, nullptr);
    auto       entity  = reinterpret_cast<YColumnMediaEntity*>(memory);
    bool       hasLrc  = false;
    static int mediaId = 0;
    mediaId--;
    entity->mId            = mediaId;
    entity->mMediaId       = QString::number(mediaId);
    entity->mOwnerId       = PLAYER_FAKE_COLUMN_ID;
    entity->mColumnId      = PLAYER_FAKE_COLUMN_ID;
    entity->mIsDir         = false;
    entity->mDownloadState = DownloadState::SUCCEED;
    entity->mTitle         = file->fileName();
    // 上一个链接不删：宿主可能还拿着它（暂停中、或两次 _play 交错），删了就成了死路。
    entity->mLocalFile = prepareTempAudioLink(file);
    // 配对歌词支持 LRC/Enhanced LRC、YRC、明文 QRC 和 TTML。
    auto       matchName = file->completeBaseName();
    double     bestScore = 0.0;
    QString    bestMatch;
    const auto entries = file->absoluteDir().entryInfoList();
    for (const auto& i : entries) {
        if (!i.isFile() || !isLyricSuffix(i.suffix())) continue;
        const double score = i.completeBaseName() == matchName ? 1.0 : fuzzyLrcMatch(matchName, i.completeBaseName());
        const auto   priority = [](const QString& path) {
            return QStringList{"lrc", "qrc", "yrc", "ttml"}.indexOf(QFileInfo(path).suffix().toLower());
        };
        if (score >= 0.35
            && (score > bestScore || (score == bestScore && priority(i.filePath()) > priority(bestMatch)))) {
            QFile candidate(i.absoluteFilePath());
            if (!candidate.open(QIODevice::ReadOnly) || candidate.size() > 2 * 1024 * 1024
                || parseLyrics(QString::fromUtf8(candidate.readAll())).isEmpty())
                continue;
            bestScore = score;
            bestMatch = i.absoluteFilePath();
        }
    }
    if (bestScore >= 0.35 && !bestMatch.isEmpty()) {
        entity->mLrcFile = bestMatch;
        hasLrc           = true;
    }
    PEN_CALL(void*, "_ZN13YMediaManager9playAudioERK18YColumnMediaEntityb", void*, YColumnMediaEntity*, bool)
    (YPointer<YMediaManager>::getInstance(), entity, true);
    loadLyrics(bestScore >= 0.35 ? bestMatch : QString());
    // 只有用户主动选曲才把播放页调到前台 —— 宿主也是这个约定（音频页列表选曲后由 QML
    // YYoudaoAudioPageColumnView.qml 自己调 YGlobal::showAudioPlayer）；曲末自动切曲弹页会打断用户当前页面。
    if (showPlayerUi) {
        PEN_CALL(void*, "_ZN7YGlobal15showAudioPlayerEv", void*)(YPointer<YGlobal>::getInstance());
    }
    delete entity;
    entity = nullptr; // entity is copied.
    if (*(PlayState*)((uintptr_t*)YPointer<YMediaPlayerManager>::getInstance() + 168) != PlayState::PLAYING) {
        PEN_CALL(void, "_ZN19YMediaPlayerManager13onClickedPlayEv", void*)
        (YPointer<YMediaPlayerManager>::getInstance());
        PEN_CALL(void, "_ZN19YMediaPlayerManager9setHasLrcEb", void*, bool)
        (YPointer<YMediaPlayerManager>::getInstance(), hasLrc);
    }
}

QString MusicPlayer::prepareTempAudioLink(const PlayFile& file) {
    mTempAudioLink.clear();
    mTempAudioTarget.clear();
    const QString original = file->absoluteFilePath();
    // 已经是 .mp3：宿主本来就能识别，不用建链
    if (file->suffix().toLower() == "mp3") {
        return original;
    }
    // 链接名必须保留 `<原名>.mp3`：宿主会用 QFileInfo(localFile).fileName() 去掉后缀来推导歌词
    // 文件名。同名不同曲靠一级哈希目录区分，不会互相覆盖（旧实现在 /tmp 下只用 completeBaseName()，
    // 两首同名歌曲会抢同一个链接）。
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(original.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    const QString linkPath = QString("%1/%2/%3.mp3").arg(tempAudioLinkRoot(), hash, file->completeBaseName());
    if (!_createTempAudioLink(linkPath, original)) {
        // 建链失败：退回原路径（宿主可能仍能播，至少不是死路）
        return original;
    }
    mTempAudioLink   = linkPath;
    mTempAudioTarget = original;
    return linkPath;
}

bool MusicPlayer::_createTempAudioLink(const QString& linkPath, const QString& target) {
    const QFileInfo linkInfo(linkPath);
    // 已经指向同一目标就直接复用：unlink/relink 之间有一小段时间路径不存在，宿主此时查
    // fileExists 会把当前媒体当成已删除。
    if (linkInfo.isSymLink() && linkInfo.symLinkTarget() == target) {
        return true;
    }
    QDir().mkpath(linkInfo.absolutePath());
    QFile::remove(linkPath);
    if (!QFile::link(target, linkPath)) {
        error("创建音频临时软链接失败: {} -> {}", linkPath.toStdString(), target.toStdString());
        return false;
    }
    debug("Created temp audio symlink: {} -> {}", linkPath.toStdString(), target.toStdString());
    return true;
}

bool MusicPlayer::repairTempAudioLink() {
    if (mTempAudioLink.isEmpty() || mTempAudioTarget.isEmpty()) {
        return false;
    }
    const QFileInfo linkInfo(mTempAudioLink);
    if (linkInfo.isSymLink() && linkInfo.exists()) {
        return true;
    }
    if (!QFileInfo::exists(mTempAudioTarget)) {
        // 原文件本身没了，补链接也救不回来
        warn("音频文件已不存在，无法修复临时软链接: {}", mTempAudioTarget.toStdString());
        return false;
    }
    if (!_createTempAudioLink(mTempAudioLink, mTempAudioTarget)) {
        return false;
    }
    info("已重建音频临时软链接: {} -> {}", mTempAudioLink.toStdString(), mTempAudioTarget.toStdString());
    return true;
}

void MusicPlayer::_purgeStaleAudioLinks() {
    QDir root(tempAudioLinkRoot());
    if (!root.exists()) {
        return;
    }
    for (const auto& sub : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir dir(root.absoluteFilePath(sub));
        for (const auto& name : dir.entryList(QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot)) {
            dir.remove(name);
        }
        dir.removeRecursively();
    }
}

void MusicPlayer::cleanupTempSymlinks() {
    if (!mTempAudioLink.isEmpty()) {
        QFile::remove(mTempAudioLink);
        debug("Cleaned up temp audio symlink: {}", mTempAudioLink.toStdString());
        mTempAudioLink.clear();
        mTempAudioTarget.clear();
    }
}

void MusicPlayer::recoverIfPlayFailed() {
    if (!mIsTakeOver) {
        return;
    }
    auto*      manager = YPointer<YMediaPlayerManager>::getInstance();
    const auto state   = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(manager);
    if (state == PlayState::PLAYING) {
        return;
    }
    // 宿主 onClickedPlay 的第一件事是 FileUtils::fileExists(localFile)：临时软链接不见了就走
    // fileNotExist() + onResetPlayer()，连原文件都不会再试，界面上按多少次都没反应。
    if (repairTempAudioLink()) {
        PEN_CALL(void, "_ZN19YMediaPlayerManager13onClickedPlayEv", void*)(manager);
        if (PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(manager) == PlayState::PLAYING) {
            info("临时软链接丢失已修复，播放恢复: {}", mTempAudioTarget.toStdString());
            return;
        }
    }
    // 仍未起来：宿主可能已经把当前媒体清空（onResetPlayer），确认之后整首重新起播一次。
    if (hostLocalFile(manager).isEmpty() && mCurrentPlaying.mFile && !mCurrentPlaying.mIsEnd) {
        warn("宿主播放未能恢复，重新起播: {}", mCurrentPlaying.mFile->fileName().toStdString());
        _play(mCurrentPlaying.mFile, false);
    }
}

void MusicPlayer::clickNext(bool showPlayerUi) {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, int64 const&)
    (YPointer<YMediaPlayerManager>::getInstance(), pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    auto newIdx = mCurrentPlaying.mIndex + 1;
    if (newIdx > mPlayList.size() - 1) newIdx = 0;
    play(newIdx, showPlayerUi);
}

void MusicPlayer::clickPrev(bool showPlayerUi) {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, void*)
    (YPointer<YMediaPlayerManager>::getInstance(), &pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    auto newIdx = mCurrentPlaying.mIndex - 1;
    if (newIdx > mPlayList.size() - 1) // overflow
        newIdx = mPlayList.size() - 1;
    play(newIdx, showPlayerUi); // back <- front
}

void MusicPlayer::clickRand(bool showPlayerUi) {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, void*)
    (YPointer<YMediaPlayerManager>::getInstance(), &pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    std::optional<size_t> newIdx;
    while (!newIdx) {
        uint32 switched = QRandomGenerator::global()->bounded((int)mPlayList.size()); // safe, limited by fm.
        if (mCurrentPlaying.mIndex != switched || mPlayList.size() == 1) {
            newIdx = switched;
            break;
        }
    }
    play(*newIdx, showPlayerUi);
}

void MusicPlayer::onSoundEnd() {
    mCurrentPlaying.mIsEnd = true;
    if (mShutdownPending && getCurrentAudioSequence() == AudioSequence::ORDER
        && mCurrentPlaying.mIndex + 1 >= mPlayList.size()) {
        cancelShutdownTimer();
        clearLyrics();
        ScreenManager::getInstance().requestPowerOff();
        return;
    }
    switch (getCurrentAudioSequence()) {
    // 曲末自动推进不是用户操作：一律不弹播放页。
    case AudioSequence::ORDER:
        clickNext(false);
        break;
    case AudioSequence::RANDOM:
        clickRand(false);
        break;
    case AudioSequence::SINGLE:
        play(mCurrentPlaying.mIndex, false);
        break;
    case AudioSequence::SINGLE_SHOT:
        auto state                                                                     = PlayState::STOPPED;
        *(uint32*)(*((uint64*)YPointer<YMediaPlayerManager>::getInstance() + 4) + 100) = 0;
        PEN_CALL(void*, "_ZN19YMediaPlayerManager12setPlayStateERKN12YEnumWrapper10Play_StateE", void*, void*)
        (YPointer<YMediaPlayerManager>::getInstance(), &state);
        clearLyrics();
        break;
    }
}

AudioSequence MusicPlayer::getCurrentAudioSequence() {
    return PEN_CALL(AudioSequence, "_ZNK15YSettingManager13audioSequenceEv", void*)(
        YPointer<YSettingManager>::getInstance()
    );
}

bool MusicPlayer::getPauseOnScan() const { return mPauseOnScan; }

void MusicPlayer::setPauseOnScan(bool enabled) {
    if (mPauseOnScan == enabled) {
        return;
    }
    mPauseOnScan         = enabled;
    auto cfg             = Config::getInstance().read("fm");
    cfg["pause_on_scan"] = enabled;
    Config::getInstance().write("fm", std::move(cfg));
    emit pauseOnScanChanged();
}

bool MusicPlayer::getHideFloatingWindow() const { return mHideFloatingWindow; }

void MusicPlayer::setHideFloatingWindow(bool hidden) {
    if (mHideFloatingWindow == hidden) {
        return;
    }
    mHideFloatingWindow         = hidden;
    auto cfg                    = Config::getInstance().read("fm");
    cfg["hide_floating_window"] = hidden;
    Config::getInstance().write("fm", std::move(cfg));
    emit hideFloatingWindowChanged();
}

bool MusicPlayer::getQuickPanelMusicView() const { return mQuickPanelMusicView; }

void MusicPlayer::setQuickPanelMusicView(bool showMusicControls) {
    if (mQuickPanelMusicView == showMusicControls) {
        return;
    }
    mQuickPanelMusicView          = showMusicControls;
    auto cfg                      = Config::getInstance().read("fm");
    cfg["quick_panel_music_view"] = showMusicControls;
    Config::getInstance().write("fm", std::move(cfg));
    emit quickPanelMusicViewChanged();
}

void MusicPlayer::setShutdownAfterPlaylist(bool enabled) {
    if (mShutdownAfterPlaylist == enabled) return;
    mShutdownAfterPlaylist = enabled;
    if (enabled) {
        // "播放结束关机" 不是倒计时模式：停止并清空定时器，立即进入等待播放列表结束的状态，
        // 这样顺序播放到最后一首结束时 onSoundEnd() 才会执行关机。
        mShutdownTimer.stop();
        mShutdownTimerMinutes = 0;
        mShutdownPending      = true;
    } else {
        mShutdownPending = false;
    }
    emit shutdownTimerChanged();
}

void MusicPlayer::setShutdownTimerMinutes(int minutes) {
    if (minutes <= 0) {
        cancelShutdownTimer();
        return;
    }
    mShutdownAfterPlaylist = false;
    mShutdownPending       = false;
    mShutdownTimerMinutes  = minutes;
    mShutdownTimer.start(minutes * 60 * 1000);
    emit shutdownTimerChanged();
}

void MusicPlayer::cancelShutdownTimer() {
    mShutdownTimer.stop();
    mShutdownPending       = false;
    mShutdownAfterPlaylist = false;
    mShutdownTimerMinutes  = 0;
    emit shutdownTimerChanged();
}

void MusicPlayer::onOcrStarted() {
    if (!mIsTakeOver) {
        return;
    }
    // Bound page-navigation protection even when an intercepted scan never emits ocrStop.
    mScanInProgress = true;
    mScanGuardTimer.start(15 * 1000);

    auto*      manager = YPointer<YMediaPlayerManager>::getInstance();
    const auto state   = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(manager);

    if (!mPauseOnScan) {
        // Do not pause, seek, restart or acquire a second MUSIC reference. The native scan-start
        // suspend hook prevents the interruption itself, rather than trying to undo it later.
        return;
    }

    if (state != PlayState::PLAYING) {
        return;
    }

    mScanPausePosition         = PEN_CALL(int64_t, "_ZNK19YMediaPlayerManager10currentPosEv", void*)(manager);
    mPausedByScan              = true;
    mPlaybackResumedDuringScan = false;
    mScanResultClosed          = false;
    PEN_CALL(void, "_ZN19YMediaPlayerManager14onClickedPauseEv", void*)(manager);

    // ocrStart 的同步 QML 处理会切换页面并停止提示音，待这些处理结束后恢复被覆盖的播放位置。
    QTimer::singleShot(0, this, &MusicPlayer::restoreScanPausePosition);
}

void MusicPlayer::onOcrStopped() {
    if (!mScanInProgress) {
        return;
    }
    mScanGuardTimer.stop();
    mScanInProgress = false;
    if (mPauseOnScan) {
        // 开关打开时暂停/恢复由 YDictPage 的 finishScanPause() 负责，这里不插手；暂停期间保持
        // MUSIC 引用是既有设计（用户可能在结果页恢复播放）。
        return;
    }
    // Hidden navigation retained the existing MUSIC reference. Release it only if music really
    // stopped; an explicit pause must remain paused, never become an automatic scan resume.
    if (!isHostPlaying()) {
        releaseAudio();
    }
}

bool MusicPlayer::isHostPlaying() const {
    const auto state = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(
        YPointer<YMediaPlayerManager>::getInstance()
    );
    return state == PlayState::PLAYING;
}

bool MusicPlayer::shouldPreventNativeScanSuspend(void* soundCenter) const {
    return mIsTakeOver && !mPauseOnScan && isHostPlaying() && hostOwnsDecoder(soundCenter);
}

void MusicPlayer::restoreScanPausePosition() {
    if (!mPausedByScan) {
        return;
    }
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    PEN_CALL(void, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, int64_t const&)(manager, mScanPausePosition);
}

int64_t MusicPlayer::normalizePositionDuringScan(int64_t requestedPosition) {
    if (requestedPosition != 0) {
        return requestedPosition;
    }
    if (mPlaybackResumedDuringScan) {
        return PEN_CALL(int64_t, "_ZNK19YMediaPlayerManager10currentPosEv", void*)(
            YPointer<YMediaPlayerManager>::getInstance()
        );
    }
    return mScanPausePosition > 0 ? mScanPausePosition : requestedPosition;
}

void MusicPlayer::onPlaybackResumedAfterScan() {
    if (!mPausedByScan) {
        return;
    }
    mPlaybackResumedDuringScan = true;
    QTimer::singleShot(0, this, [this]() {
        if (!mPausedByScan) {
            return;
        }
        restoreScanPausePosition();
        if (mScanResultClosed) {
            mPausedByScan              = false;
            mPlaybackResumedDuringScan = false;
            mScanResultClosed          = false;
        }
    });
}

bool MusicPlayer::shouldPreserveMusicOnScanResultClose() {
    if (mPausedByScan && mPlaybackResumedDuringScan) {
        // A manually resumed music session is protected only while it still owns the decoder.
        return isHostPlaying() && hostOwnsDecoder(YPointer<YSoundCenter>::getInstance());
    }
    if (mPauseOnScan || !mIsTakeOver) {
        // 开关打开（暂停由 mod 接管）或播放不是 mod 起的：沿用厂商原来的判断。
        return false;
    }
    // 开关关闭时 mPausedByScan 恒为 false，只看它会把用户正在听的后台音乐 forceStop 掉，
    // 所以现场看宿主状态。判据必须是"现在还在播"，不能是"扫描窗口开着"（返回可能晚于扫描结束）。
    return isHostPlaying() && hostOwnsDecoder(YPointer<YSoundCenter>::getInstance());
}

void MusicPlayer::finishScanPause() {
    if (!mPausedByScan) {
        return;
    }
    mScanResultClosed = true;
    if (mPlaybackResumedDuringScan) {
        // 已恢复的音乐无需再被结果页关闭流程干预，保留它的自然播放进度。
        mPausedByScan              = false;
        mPlaybackResumedDuringScan = false;
        mScanResultClosed          = false;
        return;
    }

    // 仍暂停时允许结果页关闭音效流程完成，但继续保护位置直到用户恢复播放。
    QTimer::singleShot(0, this, &MusicPlayer::restoreScanPausePosition);
}

void MusicPlayer::seekToPosition(qint64 position) {
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    if (position < 0) {
        position = 0;
    }

    const auto currentPosition = PEN_CALL(int64_t, "_ZNK19YMediaPlayerManager10currentPosEv", void*)(manager);
    const auto targetPosition  = static_cast<int64_t>(position);
    if (targetPosition > currentPosition) {
        const auto delta = targetPosition - currentPosition;
        PEN_CALL(int64_t, "_ZN19YMediaPlayerManager13onFastForwardEx", void*, int64_t)(manager, delta);
    } else if (targetPosition < currentPosition) {
        const auto delta = currentPosition - targetPosition;
        PEN_CALL(int64_t, "_ZN19YMediaPlayerManager14onFastBackwardEx", void*, int64_t)(manager, delta);
    }
}

void MusicPlayer::stop() {
    cancelOnlineLyrics();
    emit stopRequested();

    // Explicit stop ends the navigation window; nothing will automatically resume it.
    mScanInProgress = false;
    mScanGuardTimer.stop();

    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    if (!manager) {
        releaseAudio();
        return;
    }

    mCurrentPlaying.mIsEnd                    = true;
    auto state                                = PlayState::STOPPED;
    *(uint32*)(*((uint64*)manager + 4) + 100) = 0;
    // 清除状态/曲目并不会停止解码器，必须先通知 YSoundCenter 停掉实际音频。
    PEN_CALL(void, "_ZN12YSoundCenter9forceStopEv", void*)(YPointer<YSoundCenter>::getInstance());
    PEN_CALL(void*, "_ZN19YMediaPlayerManager12setPlayStateERKN12YEnumWrapper10Play_StateE", void*, void*)
    (manager, &state);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager8wipeDataEv", void*)(manager);
    // wipeData 在 seek/重同步时也会被调用，不再自动清时间轴；用户停止时在这里明确清。
    clearLyrics();
    mIsTakeOver = false;
    releaseAudio();
}

void MusicPlayer::releaseAudio() {
    mPausedByScan              = false;
    mPlaybackResumedDuringScan = false;
    mScanResultClosed          = false;
    info("QML 请求释放 MUSIC 引用");
    auto& audioDaemon = mod::AudioDaemon::getInstance();
    while (audioDaemon.sourceRefCount(AudioSource::MUSIC) > 0) {
        audioDaemon.release(AudioSource::MUSIC);
    }
}

void MusicPlayer::releaseAudioAfterHide() {
    if (mPausedByScan || mScanInProgress) {
        // 扫描暂停或扫描窗口内隐藏播放器：先保留 MUSIC 引用，等扫描结束再按正常流程释放。
        info("扫描期间隐藏播放器，保留 MUSIC 引用");
        return;
    }
    releaseAudio();
}

} // namespace mod::filemanager

// Hooks

using MusicPlayer = mod::filemanager::MusicPlayer;

namespace {

// 音乐主动起播时也必须撤销视频/插件会话，否则声音与媒体控制归属会分离。
void releaseSessionForHostPlayback() {
    auto& session = mod::MediaSession::getInstance();
    if (!session.active()) return;
    const auto owner = session.ownerPluginId();
    emit       session.stopRequested();
    session.end(owner);
}

} // namespace

PEN_HOOK(void, _ZN12YSoundCenter7suspendEv, void* self) {
#if defined(__aarch64__)
    // YDP02X: onScanStart+0x118 is BL suspend, returning to +0x11c. Match the relocated symbol
    // and validate that instruction, not an absolute address or a broad "scan in progress" flag.
    // Other firmware/layouts fail open; explicit onClickedPause, QML suspend and TTS are untouched.
    static const auto scanStart = reinterpret_cast<uintptr_t>(PEN_SYM("_ZN11YSystemBase11onScanStartEv"));
    static const auto suspend   = reinterpret_cast<uintptr_t>(PEN_SYM("_ZN12YSoundCenter7suspendEv"));
    const auto        caller = reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
    if (scanStart && suspend && caller == scanStart + 0x11c) {
        const auto callSite     = scanStart + 0x118;
        const auto expectedCall = 0x94000000u | static_cast<uint32_t>(((suspend - callSite) >> 2) & 0x03ffffffu);
        if (*reinterpret_cast<const uint32_t*>(callSite) == expectedCall
            && MusicPlayer::getInstance().shouldPreventNativeScanSuspend(self)) {
            spdlog::info("[MusicPlayer] Prevented native scan-start suspend; decoder music continues");
            return;
        }
    }
#endif
    origin(self);
}

PEN_HOOK(void*, _ZN13YMediaManager9playAudioERK18YColumnMediaEntityb, void* self, const void* entity, bool play) {
    if (play) releaseSessionForHostPlayback();
    return origin(self, entity, play);
}

PEN_HOOK(void*, _ZN13YMediaManager10clickMediaEi, void* a1, void* a2) {
    MusicPlayer::getInstance().cancelOnlineLyrics();
    MusicPlayer::mIsTakeOver = false;
    return origin(a1, a2);
}

PEN_HOOK(void, _ZN19YMediaPlayerManager13setCurrentPosERKx, void* self, int64_t const& position) {
    auto& player = MusicPlayer::getInstance();
    if (MusicPlayer::mIsTakeOver && player.isPausedByScan()) {
        const auto normalized = player.normalizePositionDuringScan(position);
        return origin(self, normalized);
    }
    return origin(self, position);
}

PEN_HOOK(void, _ZN19YMediaPlayerManager13onClickedPlayEv, void* self) {
    releaseSessionForHostPlayback();
    origin(self);
    if (MusicPlayer::mIsTakeOver) {
        MusicPlayer::getInstance().onPlaybackResumedAfterScan();
        MusicPlayer::getInstance().recoverIfPlayFailed();
    }
}

PEN_HOOK(uint64, _ZN19YMediaPlayerManager13onClickedPrevEb, void* self, bool a2) {
    if (!MusicPlayer::mIsTakeOver) return origin(self, a2);
    if (MusicPlayer::getCurrentAudioSequence() == AudioSequence::RANDOM) MusicPlayer::getInstance().clickRand();
    else MusicPlayer::getInstance().clickPrev();
    return 0;
}

PEN_HOOK(uint64, _ZN19YMediaPlayerManager13onClickedNextEb, void* self, bool a2) {
    if (!MusicPlayer::mIsTakeOver) return origin(self, a2);
    if (MusicPlayer::getCurrentAudioSequence() == AudioSequence::RANDOM) MusicPlayer::getInstance().clickRand();
    else MusicPlayer::getInstance().clickNext();
    return 0;
}

PEN_HOOK(void*, _ZN19YMediaPlayerManager10onSoundEndEj, void* self, uint32 a2) {
    if (!MusicPlayer::mIsTakeOver) return origin(self, a2);
    if (*(uint32*)(*((uint64*)self + 4) + 100) == a2 // Is current sequence equal?
        && PEN_CALL(PlayState,
                    "_ZNK19YMediaPlayerManager9playStateEv",
                    void*)(self)
               == PlayState::PLAYING) // Is in `Playing` state? To prevent unexcept onSoundEnd...
        MusicPlayer::getInstance().onSoundEnd();
    return self;
}

// wipeData 不再挂钩子：宿主在 seek、重同步和真正清空媒体时都会调用它，凭它判断“歌放完了”
// 会让一次进度跳转清掉本地时间轴（逐字歌词立刻回退成逐句、正文/译文间距同时异常）。
// 时间轴改由明确的生命周期管理：stop()/单曲播完调用 clearLyrics()，切歌由 _play() 的 loadLyrics() 覆盖。
