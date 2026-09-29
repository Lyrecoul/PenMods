// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "system/input/ScreenManager.h"
#include "system/input/InputDaemon.h"
#include "system/sound/AudioDaemon.h"

#include "filemanager/player/MusicPlayer.h"

#include "base/YEnum.h"

#include "common/Event.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMouseEvent>
#include <QQmlContext>

namespace mod {

ScreenManager::ScreenManager() {

    mCfg = Config::getInstance().read(mClassName);

    mAutoSleepDuration    = mCfg["sleep_duration"];
    mAutoShutdownDuration  = mCfg.value("shutdown_duration", 0);
    mIntelSleep           = mCfg["intel_sleep"];
    mIntelSleepAudioLock  = mCfg["intel_sleep_audio_lock"];

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("screenManager", this);

        // Install the filter on the application object so it observes events sent
        // to every receiver, including windows/scenes created by plugins.
        if (!mEventFilterInstalled) {
            if (auto* app = QCoreApplication::instance()) {
                app->installEventFilter(this);
                mEventFilterInstalled = true;
                qInfo() << "screenManager: installed global input event filter";
            }
        }
    });

    connect(&Event::getInstance(), &Event::currentPageIndexChanged, this, [this](int) {
        resetInactivityTimer();
    });
    connect(&Event::getInstance(), &Event::ocrStarted, this, [this]() {
        resetInactivityTimer();
    });

    connect(&AudioDaemon::getInstance(), &AudioDaemon::stateChanged, this, &ScreenManager::onAudioDaemonStateChanged);
    mInactivityTimer.setSingleShot(true);
    connect(&mInactivityTimer, &QTimer::timeout, this, &ScreenManager::requestPowerOff);
    if (mAutoShutdownDuration > 0) mInactivityTimer.start(mAutoShutdownDuration * 1000);
}

void ScreenManager::onPlayStateChanged(PlayState state) {
    mPlayState = state;
    rtSetAutoScreenOff(!(mPlayState == PlayState::PLAYING && mLrcShowing && mInPlayerPage));
}

void ScreenManager::onLrcShowChanged(bool show) {
    mLrcShowing = show;
    rtSetAutoScreenOff(!(mPlayState == PlayState::PLAYING && mLrcShowing && mInPlayerPage));
}

void ScreenManager::onInPlayerPageChanged(bool in) {
    mInPlayerPage = in;
    rtSetAutoScreenOff(!(mPlayState == PlayState::PLAYING && mLrcShowing && mInPlayerPage));
}

// Dynamic Actions
void ScreenManager::reportAction(const QString& action) {
    resetInactivityTimer();
    if (!getIntelSleep()) {
        return;
    }
    switch (H(action.toLocal8Bit().data())) {
    case H("wordbook_cardview_enter"):
        rtSetAutoScreenOff(false);
        break;
    case H("wordbook_cardview_quit"):
        rtSetAutoScreenOff(true);
        break;
    case H("musicplayer_lrc_show"):
        onLrcShowChanged(true);
        break;
    case H("musicplayer_lrc_hide"):
        onLrcShowChanged(false);
        break;
    }
}

QString ScreenManager::getAutoShutdownDurationStr() const {
    if (mAutoShutdownDuration == 0) return "关闭";
    return QString::number(mAutoShutdownDuration / 60) + "分钟";
}

void ScreenManager::setAutoShutdownDurationStr(const QString& str) {
    int duration = 0;
    if (str != "关闭") {
        duration = str.left(str.indexOf("分钟")).toInt() * 60;
    }
    if (duration == mAutoShutdownDuration) return;
    mAutoShutdownDuration = duration;
    mCfg["shutdown_duration"] = duration;
    WRITE_CFG;
    resetInactivityTimer();
    emit autoShutdownDurationChanged();
}

void ScreenManager::setSystemBase(YSystemBase* systemBase) {
    if (mSystemBase == nullptr && systemBase != nullptr) {
        mSystemBase = systemBase;
        qInfo() << "captured YSystemBase instance";
    }
}

bool ScreenManager::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        resetInactivityTimer();
        break;
    case QEvent::MouseMove: {
        // Only treat dragging/scrolling as activity; a bare hover would let touch
        // noise keep the pen awake forever.
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->buttons() != Qt::NoButton) resetInactivityTimer();
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ScreenManager::onScreenOn() {
    qInfo() << "screen on: reset inactivity shutdown timer";
    resetInactivityTimer();
}

void ScreenManager::onScreenOff() {
    qInfo() << "screen off: reset inactivity shutdown timer";
    resetInactivityTimer();
}

void ScreenManager::resetInactivityTimer() {
    if (mAutoShutdownDuration > 0) mInactivityTimer.start(mAutoShutdownDuration * 1000);
    else mInactivityTimer.stop();
}

void ScreenManager::requestPowerOff() {
    qInfo() << "inactivity shutdown timer expired";
    if (mSystemBase == nullptr) {
        qWarning() << "cannot power off: YSystemBase is not captured";
        return;
    }
    qInfo() << "calling YSystemBase::powerOff()";
    PEN_CALL(void*, "_ZN11YSystemBase8powerOffEv", void*)(mSystemBase);
}

QString ScreenManager::getAutoSleepDurationStr() const {
    auto dur = getAutoSleepDuration();
    if (dur == 0) {
        return "永不";
    }
    if (dur == 30) {
        return "30秒";
    }
    return QString::fromStdString(std::to_string(dur / 60) + "分钟");
}

void ScreenManager::setAutoSleepDurationStr(const QString& str) {
    int dur = 30;
    if (str == "永不") {
        dur = 0;
    }
    if (str.contains("秒")) {
        dur = (int)strtol(str.mid(0, str.indexOf("秒")).toStdString().c_str(), nullptr, 10);
    }
    if (str.contains("分")) {
        dur = (int)strtol(str.mid(0, str.indexOf("分")).toStdString().c_str(), nullptr, 10) * 60;
    }
    setAutoSleepDuration(dur);
}

int ScreenManager::getAutoSleepDuration() const { return mAutoSleepDuration; }

bool ScreenManager::getIntelSleep() const { return mIntelSleep; }

void ScreenManager::setAutoSleepDuration(int val) {
    if (mAutoSleepDuration != val) {
        mAutoSleepDuration     = val;
        mCfg["sleep_duration"] = val;
        InputDaemon::getInstance().setScreenOff(val);
        WRITE_CFG;
        emit autoSleepDurationChanged();
    }
}

void ScreenManager::setIntelSleep(bool val) {
    if (mIntelSleep != val) {
        mIntelSleep         = val;
        mCfg["intel_sleep"] = val;
        WRITE_CFG;
        emit intelSleepChanged();
    }
}

bool ScreenManager::getIntelSleepAudioLock() const { return mIntelSleepAudioLock; }

void ScreenManager::setIntelSleepAudioLock(bool val) {
    if (mIntelSleepAudioLock != val) {
        mIntelSleepAudioLock         = val;
        mCfg["intel_sleep_audio_lock"] = val;
        WRITE_CFG;
        emit intelSleepAudioLockChanged();
        onAudioDaemonStateChanged();
    }
}

void ScreenManager::onAudioDaemonStateChanged() {
    bool audioActive = AudioDaemon::getInstance().state() == AudioDaemonState::PLAYING;
    if (mIntelSleepAudioLock && audioActive && !mAudioLockActive) {
        mAudioLockActive = true;
        rtSetAutoScreenOff(false);
    } else if (mAudioLockActive && (!audioActive || !mIntelSleepAudioLock)) {
        mAudioLockActive = false;
        rtSetAutoScreenOff(true);
    }
}

void ScreenManager::rtSetAutoScreenOff(bool val) {
    val ? InputDaemon::getInstance().resume() : InputDaemon::getInstance().pause();
}

} // namespace mod

// MusicPlayer
PEN_HOOK(void*, _ZN11YSystemBase16onPowerLongPressEv, void* self) {
    mod::ScreenManager::getInstance().setSystemBase(reinterpret_cast<YSystemBase*>(self));
    return origin(self);
}

// Screen state transitions are reliable user-activity signals: waking the pen
// consumes the first tap inside the input daemon, so it never reaches Qt.
PEN_HOOK(void, _ZN11YSystemBase10onScreenOnEv, void* self) {
    mod::ScreenManager::getInstance().onScreenOn();
    origin(self);
}

PEN_HOOK(void, _ZN11YSystemBase11onScreenOffEv, void* self) {
    mod::ScreenManager::getInstance().onScreenOff();
    origin(self);
}

PEN_HOOK(uint64, _ZN7YGlobal27isInPlayerCenterPageChangedEv, uint64 self, uint64 a2, uint64 a3, uint64 a4, uint64 a5) {
    bool isInPage = PEN_CALL(bool, "_ZNK7YGlobal20isInPlayerCenterPageEv", uint64)(self);
    mod::ScreenManager ::getInstance().onInPlayerPageChanged(isInPage);
    // 离开播放器页面时，清理文件管理器创建的临时软链接
    if (!isInPage) {
        mod::filemanager::MusicPlayer::getInstance().cleanupTempSymlinks();
    }
    mod::ScreenManager::getInstance().resetInactivityTimer();
    return origin(self, a2, a3, a4, a5);
}

PEN_HOOK(
    uint64,
    _ZN19YMediaPlayerManager16playStateChangedEv,
    uint64 self,
    uint64 a2,
    uint64 a3,
    uint64 a4,
    uint64 a5
) {
    mod::ScreenManager::getInstance().onPlayStateChanged(
        PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", uint64)(self)
    );
    return origin(self, a5, a2, a3, a4);
}
