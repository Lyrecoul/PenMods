// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "filemanager/player/MusicPlayer.h"

#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "system/sound/AudioDaemon.h"

#include <QFile>
#include <QQmlContext>
#include <QRandomGenerator>

#define PLAYER_FAKE_COLUMN_ID ("fake_column_hsxjsbw")

namespace mod::filemanager {

bool MusicPlayer::mIsTakeOver{false};

MusicPlayer::MusicPlayer() : Logger("MusicPlayer") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("musicPlayer", this);
    });
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

void MusicPlayer::play(size_t idx) {
    if (idx > mPlayList.size() - 1) return;
    auto file = mPlayList.at(idx);
    if (!mCurrentPlaying.mIsEnd && mCurrentPlaying.mFile == file) {
        PEN_CALL(void*, "_ZN7YGlobal15showAudioPlayerEv", void*)(YPointer<YGlobal>::getInstance());
        return;
    }
    mCurrentPlaying.setPlaying(idx);
    _play(file);
}

void MusicPlayer::_play(const std::shared_ptr<QFileInfo>& file) {
    mIsTakeOver = true;
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
    // 清理上次的临时软链接
    cleanupTempSymlinks();
    QString lrcPath;
    entity->mLocalFile = createTempSymlinks(file, lrcPath);
    if (!lrcPath.isEmpty()) {
        entity->mLrcFile = lrcPath;
        hasLrc           = true;
    } else {
        // 后备：在原目录查找配对的 .lrc 文件（先精确匹配，再模糊匹配）
        auto       matchName = file->completeBaseName();
        double     bestScore = 0.0;
        QString    bestMatch;
        const auto entries = file->absoluteDir().entryInfoList();
        for (const auto& i : entries) {
            if (i.suffix().toLower() != "lrc") continue;
            if (i.completeBaseName() == matchName) {
                bestMatch = i.absoluteFilePath();
                bestScore = 1.0;
                break;
            }
            double score = fuzzyLrcMatch(matchName, i.completeBaseName());
            if (score > bestScore) {
                bestScore = score;
                bestMatch = i.absoluteFilePath();
            }
        }
        if (bestScore >= 0.35 && !bestMatch.isEmpty()) {
            entity->mLrcFile = bestMatch;
            hasLrc           = true;
        }
    }
    PEN_CALL(void*, "_ZN13YMediaManager9playAudioERK18YColumnMediaEntityb", void*, YColumnMediaEntity*, bool)
    (YPointer<YMediaManager>::getInstance(), entity, true);
    PEN_CALL(void*, "_ZN7YGlobal15showAudioPlayerEv", void*)(YPointer<YGlobal>::getInstance());
    delete entity;
    entity = nullptr; // entity is copied.
    if (*(PlayState*)((uintptr_t*)YPointer<YMediaPlayerManager>::getInstance() + 168) != PlayState::PLAYING) {
        PEN_CALL(void, "_ZN19YMediaPlayerManager13onClickedPlayEv", void*)
        (YPointer<YMediaPlayerManager>::getInstance());
        PEN_CALL(void, "_ZN19YMediaPlayerManager9setHasLrcEb", void*, bool)
        (YPointer<YMediaPlayerManager>::getInstance(), hasLrc);
    }
}

QString MusicPlayer::createTempSymlinks(const PlayFile& file, QString& outLrcPath) {
    outLrcPath.clear();
    const QString suffix = file->suffix().toLower();
    // 如果已经是 .mp3，无需创建软链接，直接返回原路径
    if (suffix == "mp3") {
        return file->absoluteFilePath();
    }
    // 生成唯一的临时文件名（基于原文件绝对路径的哈希）
    const QString baseName     = file->completeBaseName();
    const QString tmpAudioPath = QString("/tmp/%1.mp3").arg(baseName);
    // 创建音频文件的 .mp3 软链接
    QFile::remove(tmpAudioPath);
    if (QFile::link(file->absoluteFilePath(), tmpAudioPath)) {
        mTempAudioLink = tmpAudioPath;
        debug("Created temp audio symlink: {} -> {}",
              tmpAudioPath.toStdString(),
              file->absoluteFilePath().toStdString());
    } else {
        error("Failed to create temp audio symlink: {}", tmpAudioPath.toStdString());
        return file->absoluteFilePath();
    }
    return mTempAudioLink;
}

void MusicPlayer::cleanupTempSymlinks() {
    if (!mTempAudioLink.isEmpty()) {
        QFile::remove(mTempAudioLink);
        debug("Cleaned up temp audio symlink: {}", mTempAudioLink.toStdString());
        mTempAudioLink.clear();
    }
}

void MusicPlayer::clickNext() {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, int64 const&)
    (YPointer<YMediaPlayerManager>::getInstance(), pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    auto newIdx = mCurrentPlaying.mIndex + 1;
    if (newIdx > mPlayList.size() - 1) newIdx = 0;
    play(newIdx);
}

void MusicPlayer::clickPrev() {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, void*)
    (YPointer<YMediaPlayerManager>::getInstance(), &pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    auto newIdx = mCurrentPlaying.mIndex - 1;
    if (newIdx > mPlayList.size() - 1) // overflow
        newIdx = mPlayList.size() - 1;
    play(newIdx); // back <- front
}

void MusicPlayer::clickRand() {
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
    play(*newIdx);
}

void MusicPlayer::onSoundEnd() {
    mCurrentPlaying.mIsEnd = true;
    switch (getCurrentAudioSequence()) {
    case AudioSequence::ORDER:
        clickNext();
        break;
    case AudioSequence::RANDOM:
        clickRand();
        break;
    case AudioSequence::SINGLE:
        play(mCurrentPlaying.mIndex);
        break;
    case AudioSequence::SINGLE_SHOT:
        auto state                                                                     = PlayState::STOPPED;
        *(uint32*)(*((uint64*)YPointer<YMediaPlayerManager>::getInstance() + 4) + 100) = 0;
        PEN_CALL(void*, "_ZN19YMediaPlayerManager12setPlayStateERKN12YEnumWrapper10Play_StateE", void*, void*)
        (YPointer<YMediaPlayerManager>::getInstance(), &state);
        break;
    }
}

AudioSequence MusicPlayer::getCurrentAudioSequence() {
    return PEN_CALL(AudioSequence, "_ZNK15YSettingManager13audioSequenceEv", void*)(
        YPointer<YSettingManager>::getInstance());
}

void MusicPlayer::releaseAudio() {
    info("QML 请求释放 MUSIC 引用");
    auto& audioDaemon = mod::AudioDaemon::getInstance();
    while (audioDaemon.sourceRefCount(AudioSource::MUSIC) > 0) {
        audioDaemon.release(AudioSource::MUSIC);
    }
}

} // namespace mod::filemanager

// Hooks

using MusicPlayer = mod::filemanager::MusicPlayer;

PEN_HOOK(void*, _ZN13YMediaManager10clickMediaEi, void* a1, void* a2) {
    MusicPlayer::mIsTakeOver = false;
    return origin(a1, a2);
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

// 系统清理播放数据时，同步清理临时软链接
PEN_HOOK(void*, _ZN19YMediaPlayerManager8wipeDataEv, void* self) { return origin(self); }
