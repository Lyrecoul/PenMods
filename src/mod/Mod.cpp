// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "mod/Mod.h"

#include "chatbot/Backend.h"

#include "mod/Config.h"
#include "wallpaper/WallpaperManager.h"

#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"
#include "common/util/System.h"

#include "helper/BluetoothStorage.h"
#include "helper/CaCertificates.h"

#include "Version.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlContext>
#include <QQuickView>
#include <QTimer>

#include <unistd.h>

namespace mod {

namespace {

// 应用重启：延迟给 QML 的提示/toast 留出显示时间；看门狗用于命令没能结束进程时复位状态。
constexpr int kApplicationRestartDelayMs    = 1500;
constexpr int kApplicationRestartWatchdogMs = 10000;

// guardian 的崩溃升级阈值：/usr/bin/runDictPen 在应用退出后维护 /tmp/app_crash_count
// （连续两次退出间隔 < 15s 时 +1，否则清零），> 5 时执行 update_engine --misc=clear + reboot
// 并进入 recovery。留 2 次余量，计数偏高时拒绝本次软重启。
constexpr int kApplicationRestartCrashCountLimit = 3;

/// /tmp/app_crash_count 不存在或内容非法（老固件/其它机型）时返回 -1，调用方降级为不阻断。
int readApplicationCrashCount() {
    const std::string raw = readFile("/tmp/app_crash_count");
    if (raw.empty()) return -1;
    bool      ok    = false;
    const int count = QString::fromStdString(raw).trimmed().toInt(&ok);
    return ok && count >= 0 ? count : -1;
}

} // namespace

Mod::Mod() {

    const auto aiConfig = Config::getInstance().read("ai");
    mVoiceChatEnabled  = aiConfig.value("speech_assistant", false);

    connect(&Event::getInstance(), &Event::uiCompleted, this, &Mod::onUiCompleted);
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("mod", this);
        qmlRegisterUncreatableType<PageIndex>(
            QML_PACKAGE_NAME,
            QML_PACKAGE_VERSION_MAJOR,
            QML_PACKAGE_VERSION_MINOR,
            "PageIndex",
            "Not creatable as it is an enum type."
        );
        
        // 将 WallpaperManager 注册到 QML 上下文
        context->setContextProperty("wallpaperManager", &WallpaperManager::getInstance());
        
        // 发射属性变更信号，确保 QML 能正确获取初始值
        emit versionChanged();
        emit cachedSymCountChanged();
        emit buildInfoChanged();
    });
}

bool Mod::isTrustedDevice() const { return true; }

QString Mod::getVersionStr() const { return VERSION_STRING; }

int Mod::getCachedSymCount() const { return static_cast<int>(SymDB::getInstance().count()); }

QString Mod::getBuildInfoStr() const { return BUILD_INFO_STRING; }

QString Mod::getOtherSlot() const {
    return exec("update_engine --misc=display").find("[0]->priority = 15") != std::string::npos ? "System B"
                                                                                                : "System A";
}

void Mod::changeSlot() { exec("update_engine --misc=other --reboot"); }

void Mod::uninstall() {
    try {
        QFile modLibrary(util::getModuleFileInfo().absoluteFilePath());
        QFile mainProcess(util::getApplicationFileInfo().absoluteFilePath());
        QString appDir = util::getApplicationFileInfo().absolutePath();
        QFile mainProcessBak(appDir + "/YoudaoDictPen.original_bak");
        if (!mainProcessBak.exists()) {
            mainProcessBak.setFileName(appDir + "/YoudaoDictPen.bak");
        }
        if (!mainProcessBak.exists()) throw std::runtime_error("无法还原主程序, 因为备份已丢失");
        if (!mainProcess.remove() || !mainProcessBak.rename("YoudaoDictPen"))
            throw std::runtime_error("无法还原主程序");
        if (!modLibrary.remove()) throw std::runtime_error("无法删除 Mod 动态库，但主程序已还原");
        softReboot();
    } catch (const std::exception& e) {
        showToast(e.what(), "#E9900C");
    }
}

void Mod::softReboot() { std::terminate(); }

void Mod::reboot() { exec("sync && reboot"); }

bool Mod::isApplicationRestartPending() const { return mApplicationRestartPending; }

bool Mod::restartApplication() {
    if (mApplicationRestartPending) {
        // 防重复点击：C++ 侧状态标志，QML 侧同时禁用入口（applicationRestartPending）。
        showToast("应用正在重新启动，请稍候", "#E9900C");
        return false;
    }

    const int crashCount = readApplicationCrashCount();
    spdlog::info("Application restart requested by user (app_crash_count={})", crashCount);
    if (crashCount < 0) {
        // 文件不存在/不可读（老固件或其它机型）时降级为不阻断，只记日志。
        spdlog::info("app_crash_count is not readable, application restart guard degraded");
    } else if (crashCount >= kApplicationRestartCrashCountLimit) {
        spdlog::warn("Refusing application restart: app_crash_count={}", crashCount);
        showToast("近期重启过于频繁，请关机后再开机", "#E9900C");
        return false;
    }

    // 先落盘再重启。Config 的每次 write() 都是立即整文件写盘，没有需要额外 flush 的缓存；
    // cancelRequest() 会把正在流式输出的半截回答加入待保存快照；
    // 延迟回调在结束进程前等待后台保存并提交最新状态，
    // 同时终止在途请求，避免重启后旧回调把结果写进新进程的历史。
    chatbot::ChatBot::getInstance().cancelRequest();

    mApplicationRestartPending = true;
    emit applicationRestartPendingChanged();

    // 延迟执行：让 QML 的提示先显示出来，也避免在按钮回调里直接结束进程。
    QTimer::singleShot(kApplicationRestartDelayMs, this, &Mod::_performApplicationRestart);
    spdlog::info("Application restart scheduled in {} ms", kApplicationRestartDelayMs);
    return true;
}

void Mod::_performApplicationRestart() {
    if (!chatbot::ChatBot::getInstance().finishSessionSaves()) {
        spdlog::error("Application restart cancelled: failed to save chat history");
        mApplicationRestartPending = false;
        emit applicationRestartPendingChanged();
        showToast("聊天记录保存失败，已取消重启", "#E9900C");
        return;
    }
    // sync 必须在 kill 之前完成，而且 shell 不能随本进程一起死：QProcess::startDetached
    // 启动的子进程会被 init 收养，本进程收到 SIGTERM 后命令仍会跑完。
    const bool started = QProcess::startDetached("/bin/sh", QStringList{"-c", "sync; killall YoudaoDictPen"});
    if (!started) {
        spdlog::error("Failed to start application restart command: /bin/sh");
        mApplicationRestartPending = false;
        emit applicationRestartPendingChanged();
        showToast("重启应用失败，请手动重启设备", "#E9900C");
        return;
    }

    // pending 保持到进程真正被结束为止；如果 killall 没生效（进程名变化/命令失败），
    // 看门狗复位状态并提示，避免 UI 永远停在“正在重启”。
    QTimer::singleShot(kApplicationRestartWatchdogMs, this, &Mod::_onApplicationRestartWatchdog);
}

void Mod::_onApplicationRestartWatchdog() {
    if (!mApplicationRestartPending) return;
    spdlog::error("Application restart did not happen within {} ms", kApplicationRestartWatchdogMs);
    mApplicationRestartPending = false;
    emit applicationRestartPendingChanged();
    showToast("重启应用失败，请手动重启设备", "#E9900C");
}

bool Mod::getVoiceChatEnabled() const { return mVoiceChatEnabled; }

void Mod::setVoiceChatEnabled(bool enabled) {
    if (mVoiceChatEnabled == enabled)
        return;

    auto aiConfig = Config::getInstance().read("ai");
    aiConfig["speech_assistant"] = enabled;
    if (!Config::getInstance().write("ai", std::move(aiConfig)))
        return;
    mVoiceChatEnabled = enabled;
    emit voiceChatEnabledChanged();
}

QString Mod::getPendingVoiceChatText() const { return mPendingVoiceChatText; }

void Mod::setPendingVoiceChatText(const QString& text) {
    if (mPendingVoiceChatText == text)
        return;
    mPendingVoiceChatText = text;
    emit pendingVoiceChatTextChanged();
}

void Mod::onUiCompleted() const {

    // AutoFix vendor_storage.

    struct StoragedItem {
        QString mName;
        QString mType;
        QString mDefaultValue;
    };

    std::vector<StoragedItem> list = {
#if PL_BUILD_YDP02X
        {"VENDOR_COMPANY_ID",   "string", "COMPANY_HZ"             },
        // YDP021/022 满分版 16G
        {"VENDOR_CUSTOM_ID_0E", "string", "OVERHEAD_D2_SKU_EXA_ADV"}
    // YDP022 经典版 16G:   OVERHEAD_D2_SKU_CLA_ADV
    // YDP032 X3S 16G:     OVERHEAD_X3S_SKU_CHN_STD
    // YDP035 HLK STD:     OVERHEAD_D3_SKU_HILINK_STD
#endif
    };

    for (auto i : list) {
        if (exec(QString("vendor_storage -r %1 -t %2").arg(i.mName, i.mType)).find("vendor read error -1")
            != std::string::npos) {
            spdlog::warn("Automatically repairing vendor_storage: {}", i.mName.toStdString());
            exec(QString("vendor_storage -w %1 -t %2 -i %3").arg(i.mName, i.mType, i.mDefaultValue));
        }
    }

    // Set default read-write file system.

    exec("mount -o remount,rw /");
}

} // namespace mod

// Bypass the verification.

PEN_HOOK(bool, _ZNK15YSettingManager10isVerifiedEv, void* self) { return true; }

PEN_HOOK(bool, license_verify) { return true; }

// Setup mods.

#include "base/SymDB.h"
#include "base/YPointer.h"

#include "common/Downloader.h"
#include "common/Event.h"
#include "common/Resource.h"

#include "dict/CustomDict.h"

#include "filemanager/FileManager.h"
#include "filemanager/player/MusicPlayer.h"
#include "filemanager/player/VideoPlayer.h"
#include "filemanager/reader/TextReader.h"
#include "filemanager/viewer/ImageViewer.h"
#include "filemanager/player/ExternalPlayer.h"

#include "helper/AntiEmbs.h"
#include "helper/DeveloperSettings.h"
#include "helper/FontCache.h"
#include "helper/NetworkSettings.h"
#include "helper/ServiceManager.h"

#include "locker/Locker.h"

#include "media/MediaSession.h"

#include "mod/Updater.h"

#include "recorder/AudioRecorder.h"

#include "system/battery/BatteryInfo.h"
#include "system/input/InputDaemon.h"
#include "system/input/ScreenGrabber.h"
#include "system/input/ScreenManager.h"
#include "system/sound/ASound.h"
#include "system/sound/AudioDaemon.h"

#include "torch/Torch.h"

#include "tweaker/ColumnDBLimiter.h"
#include "tweaker/KeyBoard.h"
#include "tweaker/LoggerMonitor.h"
#include "tweaker/OcrBackend.h"
#include "tweaker/QueryTweaks.h"
#include "tweaker/TextBookHelper.h"
#include "tweaker/TouchCalibration.h"
#include "tweaker/VpuUnlock.h"
#include "tweaker/WordBookTweaks.h"

#include "hitokoto/Backend.h"

#include "chatbot/Backend.h"

#include "rime/Backend.h"

#include "plugin/PluginManager.h"

#include "capture/CameraCapture.h"

using namespace mod;

__attribute__((constructor)) static void BeforeMain() {

    // Setup global logger.

    auto global = spdlog::stdout_color_mt("Global");
#ifdef PL_DEBUG
    spdlog::set_level(spdlog::level::debug);
#endif
    spdlog::set_pattern("[%H:%M:%S.%e] [%n] [%l] %v");
    spdlog::set_default_logger(global);

    // 设备树 vpu_combo 被禁用（出厂固件）时，厂商 MPP 会在 mpp_init() 里段错误，
    // 内嵌播放器必须先被禁止尝试硬解，否则崩溃会带走整个宿主进程。
    // 详见 doc/FFMPEG_PLAYER_ANALYSIS.md。
    if (access("/dev/vpu_service", F_OK) != 0 && !qEnvironmentVariableIsSet("FFPLAYER_HWDEC")) {
        spdlog::warn("VPU is unavailable, forcing FFmpeg player software decoding");
        qputenv("FFPLAYER_HWDEC", "0");
    }

    // Setup mod instances.

#define INSTANCE(x) x::createInstance();

    // base
    INSTANCE(SymDB);
    INSTANCE(YPointerInitializer);

    // mod
    INSTANCE(Config);
    INSTANCE(Mod);
    INSTANCE(Updater);

    // common
    INSTANCE(Downloader);
    INSTANCE(Event);
    INSTANCE(Resource);

    // media
    INSTANCE(MediaSession);

    // filemanager
    INSTANCE(filemanager::MusicPlayer);
    INSTANCE(filemanager::VideoPlayer);
    INSTANCE(filemanager::TextReader);
    INSTANCE(filemanager::FileManager);
    INSTANCE(filemanager::ImageViewer);
    INSTANCE(filemanager::ExternalPlayer);

    // helper
    INSTANCE(AntiEmbs);
    INSTANCE(BluetoothStorage);
    INSTANCE(CaCertificates);
    INSTANCE(DeveloperSettings);
    INSTANCE(FontCache);
    INSTANCE(NetworkSettings);
    INSTANCE(ServiceManager);

    // locker
    INSTANCE(Locker);

    // dict
    INSTANCE(CustomDict);

    // recorder
    INSTANCE(AudioRecorder);

    // system
    INSTANCE(BatteryInfo);
    INSTANCE(InputDaemon);
    INSTANCE(ScreenGrabber);
    INSTANCE(ScreenManager);
    INSTANCE(ASound);
    INSTANCE(AudioDaemon);

    // torch
    INSTANCE(Torch);

    // tweaker
    INSTANCE(ColumnDBLimiter);
    INSTANCE(KeyBoard);
    INSTANCE(LoggerMonitor);
    INSTANCE(OcrBackend);
    INSTANCE(QueryTweaks);
    INSTANCE(TextBookHelper);
    INSTANCE(TouchCalibration);
    INSTANCE(VpuUnlock);
    INSTANCE(WordBookTweaks);

    // hitokoto
    INSTANCE(hitokoto::Hitokoto)

    // chatbot
    INSTANCE(chatbot::ChatBot)

    // rime
    INSTANCE(rime::Backend)

    // plugin
    INSTANCE(PluginManager)
    
    // wallpaper
    INSTANCE(WallpaperManager)

    // capture
    INSTANCE(capture::CameraCapture)

#undef INSTANCE
}
