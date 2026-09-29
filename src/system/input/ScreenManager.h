// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "base/YEnum.h"
#include "base/YPointer.h"

#include "common/Utils.h"

#include "mod/Config.h"

#include <QEvent>
#include <QTimer>

namespace mod {

class ScreenManager : public QObject, public Singleton<ScreenManager> {
    Q_OBJECT

    Q_PROPERTY(QString autoShutdownDuration READ getAutoShutdownDurationStr WRITE setAutoShutdownDurationStr NOTIFY
                   autoShutdownDurationChanged);
    Q_PROPERTY(int intelSleep READ getIntelSleep WRITE setIntelSleep NOTIFY intelSleepChanged);
    Q_PROPERTY(bool intelSleepAudioLock READ getIntelSleepAudioLock WRITE setIntelSleepAudioLock NOTIFY
                   intelSleepAudioLockChanged);

public:
    QString getAutoSleepDurationStr() const;
    QString getAutoShutdownDurationStr() const;

    int getAutoSleepDuration() const;

    bool getIntelSleep() const;

    bool getIntelSleepAudioLock() const;

    void setAutoSleepDurationStr(const QString&);
    void setAutoShutdownDurationStr(const QString&);

    void setAutoSleepDuration(int);

    void setIntelSleep(bool);

    void setIntelSleepAudioLock(bool);

    Q_INVOKABLE void reportAction(const QString&);

    // Captures every input event in the application (including plugin pages) so
    // that any real user interaction resets the inactivity shutdown timer.
    bool eventFilter(QObject* watched, QEvent* event) override;

    // runtime
    void rtSetAutoScreenOff(bool);

    void onPlayStateChanged(PlayState);

    void onLrcShowChanged(bool);

    void onInPlayerPageChanged(bool);

    void onAudioDaemonStateChanged();
    void setSystemBase(YSystemBase* systemBase);
    void resetInactivityTimer();
    void requestPowerOff();

    // Screen state transitions are treated as user activity as well: the tap that
    // wakes the pen is consumed by the input daemon and may never reach Qt, so a
    // screen-on event is the only reliable "wake" notification.
    void onScreenOn();
    void onScreenOff();

signals:

    void autoSleepDurationChanged();
    void autoShutdownDurationChanged();

    void intelSleepChanged();

    void intelSleepAudioLockChanged();

private:
    friend Singleton<ScreenManager>;
    explicit ScreenManager();

    std::string mClassName = "screen";
    json        mCfg;

    int  mAutoSleepDuration;
    int  mAutoShutdownDuration{0};
    QTimer mInactivityTimer;
    bool mIntelSleep;
    bool mIntelSleepAudioLock;
    bool mAudioLockActive = false;

    YSystemBase* mSystemBase = nullptr;

    bool mEventFilterInstalled = false;

    // Tmp saving;
    bool      mLrcShowing   = false;
    bool      mInPlayerPage = false;
    PlayState mPlayState    = PlayState::STOPPED;
};

} // namespace mod
