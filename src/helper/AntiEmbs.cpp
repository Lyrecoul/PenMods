// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "AntiEmbs.h"

#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "mod/Mod.h"

#include "filemanager/FileManager.h"
#include "locker/Locker.h"

#include "system/sound/ASound.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QQmlContext>

namespace mod {

AntiEmbs::AntiEmbs() {

    mCfg = Config::getInstance().read(mClassName);

    mAutoMute       = mCfg["auto_mute"];
    mAutoPronLocked = mCfg["no_auto_pron"];
    mFastHide       = mCfg["fast_hide_music"];
    mLowVoiceMode   = mCfg["low_voice"];
    mFastMute       = mCfg["fast_mute"];
    // 旧配置里没有这个键时不能直接取值：mCfg 是本类的配置节，_fill_missing_defaults
    // 会在加载时补齐，但为兼容被外部手改过的配置，这里仍然走一次存在性判断。
    mBootMute = mCfg.contains("boot_mute") ? mCfg["boot_mute"].get<bool>() : false;

    connect(this, &AntiEmbs::lowVoiceModeChanged, [&]() {
        mLowVoiceMode ? ASound::getInstance().setDb({-80.0, -20.0}) : ASound::getInstance().setDb({-50.0, 0.0});
        exec("killall SoundPlayer");
    });

    connect(this, &AntiEmbs::autoPronLockChanged, [&]() {
        if (mAutoPronLocked) {
            PEN_CALL(void*, "_ZN15YSettingManager16setAutoPronounceEb", void*, bool)
            (YPointer<YSettingManager>::getInstance(), false);
            PEN_CALL(void*, "_ZN15YSettingManager20setIsWbAutoPronounceEb", void*, bool)
            (YPointer<YSettingManager>::getInstance(), false);
        }
    });

    connect(&Event::getInstance(), &Event::uiCompleted, this, &AntiEmbs::onUiCompleted);
    connect(&Event::getInstance(), &Event::homeButtonPressed, this, &AntiEmbs::onHomeButtonPress);

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("antiEmbs", this);
    });
}

void AntiEmbs::onUiCompleted() {
    emit lowVoiceModeChanged();
    emit autoPronLockChanged();

    // 开机默认静音：只在 UI 就绪后做一次。厂商在自身初始化阶段会把上次保存的音量
    // 恢复回来，uiCompleted 是那之后第一个我们确定已经跑完的点。
    // 两个入口都要写：YSettingManager::setSpkVolume 的缓存值已经等于目标值时会提前
    // 返回，真正下发到混音器的是 YDeviceManager::setSpkVolume（与
    // VolumeGuard::enforce() 的处理方式一致）。写 0 不需要延迟重试。
    // 三个厂商符号都先判空再调：PEN_CALL 是裸函数指针强转，符号缺失时等于跳转到 0。
    // 这里在 uiCompleted（Qt 槽）里执行，一次空指针调用就是一次 SIGSEGV -> 守护器重启 ->
    // 连续快速失败会把设备推进 recovery，代价远大于"静音功能不生效"。
    if (mBootMute) {
        auto* setting = YPointer<YSettingManager>::getInstance();
        auto* setVolumeSym = PEN_SYM("_ZN15YSettingManager12setSpkVolumeEi");
        if (setting == nullptr || setVolumeSym == nullptr) {
            spdlog::warn("AntiEmbs: 开机默认静音跳过（YSettingManager 或 setSpkVolume 不可用）");
            return;
        }
        reinterpret_cast<void (*)(void*, int)>(setVolumeSym)(setting, 0);

        auto* instanceSym = PEN_SYM("_ZN10YSingletonI14YDeviceManagerE8instanceEv");
        auto* deviceSym   = PEN_SYM("_ZN14YDeviceManager12setSpkVolumeEi");
        if (instanceSym != nullptr && deviceSym != nullptr) {
            auto* device = reinterpret_cast<void* (*)()>(instanceSym)();
            if (device != nullptr) reinterpret_cast<void (*)(void*, int)>(deviceSym)(device, 0);
        }
        spdlog::info("AntiEmbs: 开机默认静音已应用（音量=0）");
    }
}

void AntiEmbs::onHomeButtonPress() {
    if (!getFastHide()) {
        return;
    }
    auto ts = QDateTime::currentDateTime().toSecsSinceEpoch();
    mHomePressTimes.emplace_back(ts);
    if (mHomePressTimes.size() < 4) {
        return;
    }
    while (mHomePressTimes.size() > 4) {
        mHomePressTimes.erase(mHomePressTimes.begin());
    }
    for (auto i : mHomePressTimes) {
        if (ts - i > 3) {
            return;
        }
    }
    mHomePressTimes.clear();
    if (isActive() && Locker::getInstance().getScene("antiembs_deactivate")) {
        emit deactivationUnlockRequested();
        return;
    }
    showToast("操作执行完毕");
    filemanager::FileManager::getInstance().negateHiddenAll();
    emit activeChanged();
}

bool AntiEmbs::deactivate(const QString& password) {
    if (!isActive()) {
        return true;
    }
    auto& locker = Locker::getInstance();
    if (locker.getScene("antiembs_deactivate") && password != locker.getPassword()) {
        return false;
    }
    showToast("操作执行完毕");
    filemanager::FileManager::getInstance().negateHiddenAll();
    emit activeChanged();
    return true;
}

bool AntiEmbs::getAutoMute() const { return mAutoMute; }

bool AntiEmbs::getBootMute() const { return mBootMute; }

bool AntiEmbs::getLowVoiceMode() const { return mLowVoiceMode; }

bool AntiEmbs::getAutoPronLocked() const { return mAutoPronLocked; }

bool AntiEmbs::getFastHide() const { return mFastHide && Mod::getInstance().isTrustedDevice(); }

bool AntiEmbs::isActive() const {
    return Mod::getInstance().isTrustedDevice() && filemanager::FileManager::getInstance().shouldHiddenAll();
}

bool AntiEmbs::getFastMute() const { return mFastMute; }

void AntiEmbs::setLowVoiceMode(bool val) {
    if (mLowVoiceMode != val) {
        mLowVoiceMode = val;
        UPDATE_CFG("low_voice", val);
        emit lowVoiceModeChanged();
    }
}

void AntiEmbs::setAutoMute(bool val) {
    if (mAutoMute != val) {
        mAutoMute = val;
        UPDATE_CFG("auto_mute", val);
        emit autoMuteChanged();
    }
}

void AntiEmbs::setFastMute(bool val) {
    if (mFastMute != val) {
        mFastMute = val;
        UPDATE_CFG("fast_mute", val);
        emit fastMuteChanged();
    }
}

void AntiEmbs::setAutoPronLocked(bool val) {
    if (mAutoPronLocked != val) {
        mAutoPronLocked = val;
        UPDATE_CFG("no_auto_pron", val);
        emit autoPronLockChanged();
    }
}

void AntiEmbs::setFastHide(bool val) {
    if (mFastHide != val) {
        mFastHide = val;
        UPDATE_CFG("fast_hide_music", val);
        emit fastHideChanged();
    }
}

void AntiEmbs::setBootMute(bool val) {
    if (mBootMute != val) {
        mBootMute = val;
        UPDATE_CFG("boot_mute", val);
        emit bootMuteChanged();
    }
}

} // namespace mod

PEN_HOOK(uint64, _ZN17YBlueToothManager17onBtStatusChangedEb, uint64 self, bool a2) {
    if (!a2 && mod::AntiEmbs::getInstance().getAutoMute()) {
        PEN_CALL(void*, "_ZN15YSettingManager12setSpkVolumeEi", void*, int)
        (mod::YPointer<YSettingManager>::getInstance(), 0);
    }
    return origin(self, a2);
}

PEN_HOOK(uint64, _ZN15YSettingManager16setAutoPronounceEb, uint64 self, bool a2) {
    if (mod::AntiEmbs::getInstance().getAutoPronLocked()) {
        a2 = false;
    }
    return origin(self, a2);
}

PEN_HOOK(uint64, _ZN15YSettingManager20setIsWbAutoPronounceEb, uint64 self, bool a2) {
    if (mod::AntiEmbs::getInstance().getAutoPronLocked()) {
        a2 = false;
    }
    return origin(self, a2);
}
