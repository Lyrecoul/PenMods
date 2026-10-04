// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "media/MediaSession.h"

#include "filemanager/player/MusicPlayer.h"

#include "common/Event.h"

#include "plugin/PluginSDK.h"

#include "Version.h"

#include <QMutex>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>

#include <algorithm>
#include <cstring>
#include <limits>

namespace mod {

namespace {

// ------------------------------------------------------------------
// 插件 C ABI
//
// 插件可能在任意线程调用，这里统一把状态变更 marshal 到 UI 线程；
// handle 是一次 begin/end 周期内的不透明令牌，用来丢弃过期插件的调用。
//
// 令牌是单调递增的整数（编码在 void* 里）而不是指针：指针版本会在接管时
// delete + new，分配器复用同一地址，于是被接管插件的陈旧 handle 仍然“存活”，
// 能误操作新属主的会话。整数令牌永不复用，天然作废。
//
// 状态全部放在函数内静态对象里：插件是在 BeforeMain 里加载的，文件级静态对象
// （尤其是 QString 这种非平凡构造的）此时可能还没构造，直接用会空指针崩溃。
// ------------------------------------------------------------------

struct MediaApiState {
    uint64_t             nextHandle{0};    // 只增不减
    uint64_t             currentHandle{0}; // 0 表示当前没有发出去的 C 侧 handle
    QString              handlePluginId;
    QMutex               mutex;
    PluginMediaCallbacks callbacks{};
    void*                callbackUser{nullptr};
    bool                 hasCallbacks{false};
};

MediaApiState& state() {
    static MediaApiState s;
    return s;
}

template <typename F>
void postToUi(F&& fn) {
    QMetaObject::invokeMethod(&MediaSession::getInstance(), std::forward<F>(fn), Qt::QueuedConnection);
}

void* toHandle(uint64_t token) { return reinterpret_cast<void*>(static_cast<uintptr_t>(token)); }

/// 需持有 state().mutex
bool handleAliveLocked(void* handle) {
    return handle != nullptr && static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)) == state().currentHandle;
}

bool handleAlive(void* handle) {
    QMutexLocker locker(&state().mutex);
    return handleAliveLocked(handle);
}

/// 会话结束后作废 C 侧 handle。
void clearHandle() {
    QMutexLocker locker(&state().mutex);
    state().currentHandle = 0;
    state().handlePluginId.clear();
}

/// 会话易主时调用：C 侧 handle 不属于新属主就作废，否则旧属主的上报仍会被当成当前会话接收。
void discardHandleUnlessOwnedBy(const QString& pluginId) {
    QMutexLocker locker(&state().mutex);
    if (state().currentHandle == 0 || state().handlePluginId == pluginId) {
        return;
    }
    state().currentHandle = 0;
    state().handlePluginId.clear();
}

void* apiBeginSession(const char* pluginId) {
    const QString id = pluginId ? QString::fromUtf8(pluginId) : QString();
    if (id.isEmpty()) {
        return nullptr;
    }

    QMutexLocker locker(&state().mutex);
    if (state().currentHandle != 0 && state().handlePluginId == id) {
        return toHandle(state().currentHandle);
    }
    state().currentHandle  = ++state().nextHandle;
    state().handlePluginId = id;
    const uint64_t token   = state().currentHandle;
    locker.unlock();

    postToUi([id] { MediaSession::getInstance().begin(id); });
    return toHandle(token);
}

void apiEndSession(void* handle) {
    {
        QMutexLocker locker(&state().mutex);
        if (!handleAliveLocked(handle)) {
            return;
        }
        state().currentHandle = 0;
        state().handlePluginId.clear();
    }
    postToUi([] { MediaSession::getInstance().end(); });
}

void apiSetTrack(void* handle, const char* title, const char* artist, const char* album, const char* cover) {
    if (!handleAlive(handle)) {
        return;
    }
    const QString t  = title ? QString::fromUtf8(title) : QString();
    const QString ar = artist ? QString::fromUtf8(artist) : QString();
    const QString al = album ? QString::fromUtf8(album) : QString();
    const QString cv = cover ? QString::fromUtf8(cover) : QString();
    postToUi([t, ar, al, cv] {
        auto& session = MediaSession::getInstance();
        session.setTitle(t);
        session.setArtist(ar);
        session.setAlbum(al);
        session.setCover(cv);
    });
}

void apiSetPlayState(void* handle, int state) {
    if (!handleAlive(handle) || state < MediaSession::Stopped || state > MediaSession::Paused) {
        return;
    }
    postToUi([state] { MediaSession::getInstance().setPlayState(static_cast<MediaSession::PlayState>(state)); });
}

void apiSetPosition(void* handle, int64_t positionMs) {
    if (!handleAlive(handle)) {
        return;
    }
    const int clamped = static_cast<int>(std::clamp<int64_t>(positionMs, 0, std::numeric_limits<int>::max()));
    postToUi([clamped] { MediaSession::getInstance().setPosition(clamped); });
}

void apiSetDuration(void* handle, int64_t durationMs) {
    if (!handleAlive(handle)) {
        return;
    }
    const int clamped = static_cast<int>(std::clamp<int64_t>(durationMs, 0, std::numeric_limits<int>::max()));
    postToUi([clamped] { MediaSession::getInstance().setDuration(clamped); });
}

void apiSetLyrics(void* handle, const char* mainLyric, const char* transLyric) {
    if (!handleAlive(handle)) {
        return;
    }
    const QString main  = mainLyric ? QString::fromUtf8(mainLyric) : QString();
    const QString trans = transLyric ? QString::fromUtf8(transLyric) : QString();
    postToUi([main, trans] { MediaSession::getInstance().setLyrics(main, trans); });
}

int apiSetCallbacks(void* handle, const PluginMediaCallbacks* callbacks, void* user) {
    if (!handleAlive(handle)) {
        return -1;
    }
    PluginMediaCallbacks copy{};
    if (callbacks && callbacks->structSize >= sizeof(uint32_t)) {
        std::memcpy(&copy, callbacks, std::min<size_t>(callbacks->structSize, sizeof(PluginMediaCallbacks)));
    }
    const bool has = callbacks != nullptr;
    // 回调状态只在 UI 线程读写（dispatch* 也在 UI 线程）。
    postToUi([copy, has, user] {
        state().callbacks    = copy;
        state().callbackUser = user;
        state().hasCallbacks = has;
    });
    return 0;
}

void dispatchPlay() {
    if (state().hasCallbacks && state().callbacks.onPlay) state().callbacks.onPlay(state().callbackUser);
}
void dispatchPause() {
    if (state().hasCallbacks && state().callbacks.onPause) state().callbacks.onPause(state().callbackUser);
}
void dispatchToggle() {
    if (state().hasCallbacks && state().callbacks.onToggle) state().callbacks.onToggle(state().callbackUser);
}
void dispatchNext() {
    if (state().hasCallbacks && state().callbacks.onNext) state().callbacks.onNext(state().callbackUser);
}
void dispatchPrev() {
    if (state().hasCallbacks && state().callbacks.onPrev) state().callbacks.onPrev(state().callbackUser);
}
void dispatchStop() {
    if (state().hasCallbacks && state().callbacks.onStop) state().callbacks.onStop(state().callbackUser);
}
void dispatchSeek(int position) {
    if (state().hasCallbacks && state().callbacks.onSeek) state().callbacks.onSeek(state().callbackUser, position);
}
void dispatchOpen() {
    if (state().hasCallbacks && state().callbacks.onOpen) state().callbacks.onOpen(state().callbackUser);
}

void clearCallbacks() {
    state().hasCallbacks = false;
    state().callbackUser = nullptr;
    state().callbacks    = {};
}

// 会话被接管：先通知旧属主，然后清掉它的回调。会话已经易主，这些回调不再有效，
// 留着会让下一次接管把它们误当成当前属主。
void dispatchRevoked() {
    if (state().hasCallbacks && state().callbacks.onSessionRevoked) {
        state().callbacks.onSessionRevoked(state().callbackUser);
    }
    clearCallbacks();
}

} // namespace

MediaSession::MediaSession() : Logger("MediaSession") {

    mPositionTimer.setInterval(500);
    connect(&mPositionTimer, &QTimer::timeout, this, [this] {
        if (!mActive || mPlayState != Playing) {
            return;
        }
        if (mDuration > 0 && mPosition >= mDuration) {
            return;
        }
        mPosition += mPositionTimer.interval();
        if (mDuration > 0) {
            mPosition = std::min(mPosition, mDuration);
        }
        emit positionChanged();
    });

    // C ABI 回调派发：面板发射信号时同步转发给插件。
    connect(this, &MediaSession::playRequested, this, &dispatchPlay);
    connect(this, &MediaSession::pauseRequested, this, &dispatchPause);
    connect(this, &MediaSession::toggleRequested, this, &dispatchToggle);
    connect(this, &MediaSession::nextRequested, this, &dispatchNext);
    connect(this, &MediaSession::prevRequested, this, &dispatchPrev);
    connect(this, &MediaSession::stopRequested, this, &dispatchStop);
    connect(this, &MediaSession::openRequested, this, &dispatchOpen);
    connect(this, &MediaSession::seekRequested, this, &dispatchSeek);

    // 会话被接管时通知旧属主的 onSessionRevoked（没有注册则该回调为空）。
    connect(this, &MediaSession::sessionRevoked, this, &dispatchRevoked);

    // 会话结束后清掉 C ABI 回调与 handle，否则插件被卸载后面板再发控制事件就是野指针。
    connect(this, &MediaSession::activeChanged, this, [] {
        if (MediaSession::getInstance().active()) {
            return;
        }
        clearCallbacks();
        clearHandle();
    });

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("mediaSession", this);
        qmlRegisterUncreatableType<MediaSession>(
            QML_PACKAGE_NAME,
            QML_PACKAGE_VERSION_MAJOR,
            QML_PACKAGE_VERSION_MINOR,
            "MediaSession",
            "Not creatable as it is an enum type."
        );
    });
}

void MediaSession::reset() {
    mTitle.clear();
    mArtist.clear();
    mAlbum.clear();
    mCover.clear();
    mDuration  = 0;
    mPosition  = 0;
    mPlayState = Stopped;
    mMainLyric.clear();
    mTransLyric.clear();
    syncPositionTimer();

    emit titleChanged();
    emit artistChanged();
    emit albumChanged();
    emit coverChanged();
    emit durationChanged();
    emit positionChanged();
    emit playStateChanged();
    emit lyricsChanged();
}

void MediaSession::syncPositionTimer() {
    if (mActive && mPlayState == Playing) {
        if (!mPositionTimer.isActive()) {
            mPositionTimer.start();
        }
    } else {
        mPositionTimer.stop();
    }
}

bool MediaSession::begin(const QString& pluginId) {
    if (pluginId.isEmpty()) {
        return false;
    }
    if (mActive && mOwnerPluginId == pluginId) {
        return true;
    }

    // C 侧 handle 必须属于当前属主：被 QML 插件接管后，旧 C 插件的 handle 立即作废。
    // （C -> C 的接管由 apiBeginSession 同步换发新令牌，这里会保留它。）
    discardHandleUnlessOwnedBy(pluginId);

    if (mActive) {
        info("Media session '{}' taken over by '{}'.", mOwnerPluginId.toStdString(), pluginId.toStdString());
        // 收到 sessionRevoked 的必然是旧属主。它此时调 end() 会误关即将接管的新会话，
        // 所以在广播期间把无条件 end() 屏蔽掉（属主校验的 end(pluginId) 本来就会返回 false）。
        mRevoking = true;
        emit sessionRevoked();
        mRevoking = false;
    }

    mOwnerPluginId = pluginId;
    reset();
    mActive = true;
    emit activeChanged();

    // 插件开始接管时停掉宿主播放器，避免两路声音和两套面板状态。
    filemanager::MusicPlayer::getInstance().stop();

    info("Media session opened by '{}'.", pluginId.toStdString());
    return true;
}

void MediaSession::end() {
    if (!mActive) {
        return;
    }
    if (mRevoking) {
        warn("Ignoring end() from a revoked owner; use end(pluginId) instead.");
        return;
    }
    closeSession();
}

bool MediaSession::end(const QString& pluginId) {
    if (!mActive || mRevoking || pluginId.isEmpty() || pluginId != mOwnerPluginId) {
        return false;
    }
    closeSession();
    return true;
}

void MediaSession::closeSession() {
    info("Media session closed by '{}'.", mOwnerPluginId.toStdString());
    mActive = false;
    mOwnerPluginId.clear();
    reset();
    emit activeChanged();
}

void MediaSession::releaseFor(const QString& pluginId) {
    if (mActive && mOwnerPluginId == pluginId) {
        info("Releasing media session of unloaded plugin '{}'.", pluginId.toStdString());
        end();
    }
}

void MediaSession::setLyrics(const QString& mainLyric, const QString& transLyric) {
    if (mMainLyric == mainLyric && mTransLyric == transLyric) {
        return;
    }
    mMainLyric  = mainLyric;
    mTransLyric = transLyric;
    emit lyricsChanged();
}

void MediaSession::setTitle(const QString& value) {
    if (mTitle == value) return;
    mTitle = value;
    emit titleChanged();
}

void MediaSession::setArtist(const QString& value) {
    if (mArtist == value) return;
    mArtist = value;
    emit artistChanged();
}

void MediaSession::setAlbum(const QString& value) {
    if (mAlbum == value) return;
    mAlbum = value;
    emit albumChanged();
}

void MediaSession::setCover(const QString& value) {
    if (mCover == value) return;
    mCover = value;
    emit coverChanged();
}

void MediaSession::setDuration(int value) {
    value = std::max(0, value);
    if (mDuration == value) return;
    mDuration = value;
    if (mDuration > 0 && mPosition > mDuration) {
        mPosition = mDuration;
        emit positionChanged();
    }
    emit durationChanged();
}

void MediaSession::setPosition(int value) {
    value = std::max(0, value);
    if (mDuration > 0) {
        value = std::min(value, mDuration);
    }
    if (mPosition == value) return;
    mPosition = value;
    emit positionChanged();
}

void MediaSession::setPlayState(PlayState value) {
    if (mPlayState == value) return;
    mPlayState = value;
    syncPositionTimer();
    emit playStateChanged();
}

void MediaSession::setMainLyric(const QString& value) {
    if (mMainLyric == value) return;
    mMainLyric = value;
    emit lyricsChanged();
}

void MediaSession::setTransLyric(const QString& value) {
    if (mTransLyric == value) return;
    mTransLyric = value;
    emit lyricsChanged();
}

PluginMediaAPI* MediaSession::pluginApi() {
    static PluginMediaAPI api = {
        sizeof(PluginMediaAPI),
        PLUGIN_MEDIA_ABI_VERSION,
        &apiBeginSession,
        &apiEndSession,
        &apiSetTrack,
        &apiSetPlayState,
        &apiSetPosition,
        &apiSetDuration,
        &apiSetLyrics,
        &apiSetCallbacks,
    };
    return &api;
}

} // namespace mod
