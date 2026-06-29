// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "base/YEnum.h"

#include "common/Utils.h"

#include "mod/Config.h"

namespace mod {

class ScreenManager : public QObject, public Singleton<ScreenManager> {
    Q_OBJECT

    Q_PROPERTY(QString autoSleepDuration READ getAutoSleepDurationStr WRITE setAutoSleepDurationStr NOTIFY
                   autoSleepDurationChanged);
    Q_PROPERTY(int intelSleep READ getIntelSleep WRITE setIntelSleep NOTIFY intelSleepChanged);
    Q_PROPERTY(bool intelSleepAudioLock READ getIntelSleepAudioLock WRITE setIntelSleepAudioLock NOTIFY
                   intelSleepAudioLockChanged);

public:
    QString getAutoSleepDurationStr() const;

    int getAutoSleepDuration() const;

    bool getIntelSleep() const;

    bool getIntelSleepAudioLock() const;

    void setAutoSleepDurationStr(const QString&);

    void setAutoSleepDuration(int);

    void setIntelSleep(bool);

    void setIntelSleepAudioLock(bool);

    Q_INVOKABLE void reportAction(const QString&);

    // runtime
    void rtSetAutoScreenOff(bool);

    void onPlayStateChanged(PlayState);

    void onLrcShowChanged(bool);

    void onInPlayerPageChanged(bool);

    void onAudioDaemonStateChanged();

signals:

    void autoSleepDurationChanged();

    void intelSleepChanged();

    void intelSleepAudioLockChanged();

private:
    friend Singleton<ScreenManager>;
    explicit ScreenManager();

    std::string mClassName = "screen";
    json        mCfg;

    int  mAutoSleepDuration;
    bool mIntelSleep;
    bool mIntelSleepAudioLock;
    bool mAudioLockActive = false;

    // Tmp saving;
    bool      mLrcShowing   = false;
    bool      mInPlayerPage = false;
    PlayState mPlayState    = PlayState::STOPPED;
};

} // namespace mod
