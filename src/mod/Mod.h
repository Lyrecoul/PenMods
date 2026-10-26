// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

namespace mod {

struct PageIndex {
    enum Enum { _ModPage = 100, AudioRecorder, VideoPlayer, ExternalPlayer, ChatAssistant, PluginManager, WallpaperSetting };
    Q_ENUM(Enum)
    Q_GADGET
};

class Mod : public QObject, public Singleton<Mod> {
    Q_OBJECT

    Q_PROPERTY(bool trustedDevice READ isTrustedDevice);
    Q_PROPERTY(QString version READ getVersionStr NOTIFY versionChanged);
    Q_PROPERTY(int cachedSymCount READ getCachedSymCount NOTIFY cachedSymCountChanged);
    Q_PROPERTY(QString buildInfo READ getBuildInfoStr NOTIFY buildInfoChanged)
    Q_PROPERTY(bool voiceChatEnabled READ getVoiceChatEnabled WRITE setVoiceChatEnabled NOTIFY voiceChatEnabledChanged)
    Q_PROPERTY(QString pendingVoiceChatText READ getPendingVoiceChatText WRITE setPendingVoiceChatText NOTIFY pendingVoiceChatTextChanged)
    Q_PROPERTY(bool applicationRestartPending READ isApplicationRestartPending NOTIFY applicationRestartPendingChanged)

public:
    // 为属性变更添加信号
    Q_SIGNAL void versionChanged();
    Q_SIGNAL void cachedSymCountChanged();
    Q_SIGNAL void buildInfoChanged();
    Q_SIGNAL void voiceChatEnabledChanged();
    Q_SIGNAL void pendingVoiceChatTextChanged();
    Q_SIGNAL void applicationRestartPendingChanged();

    void onUiCompleted() const;

    [[nodiscard]] bool isTrustedDevice() const;

    [[nodiscard]] QString getVersionStr() const;

    [[nodiscard]] int getCachedSymCount() const;

    [[nodiscard]] QString getBuildInfoStr() const;

    Q_INVOKABLE [[nodiscard]] QString getOtherSlot() const;

    Q_INVOKABLE void changeSlot();

    Q_INVOKABLE void uninstall();

    // 旧语义：异常结束当前进程（uninstall 依赖），保留不动。
    Q_INVOKABLE void softReboot();

    // 重启整机（Linux reboot），与应用进程重启区分开。
    Q_INVOKABLE void reboot();

    /// 受控地重启 YoudaoDictPen 应用进程（不是整机 reboot）。
    /// 会先把内存状态落盘、延迟约 1.5s 后执行 `sync; killall YoudaoDictPen`，
    /// 由 guardian_run 重新拉起应用。首次调用返回 true；已有待执行的重启时返回 false。
    Q_INVOKABLE bool restartApplication();

    [[nodiscard]] bool isApplicationRestartPending() const;

    bool getVoiceChatEnabled() const;
    void setVoiceChatEnabled(bool enabled);
    QString getPendingVoiceChatText() const;
    void setPendingVoiceChatText(const QString& text);

private:
    friend Singleton<Mod>;
    explicit Mod();

    void _performApplicationRestart();
    void _onApplicationRestartWatchdog();

    std::string mClassName        = "mod";
    bool        mVoiceChatEnabled = false;
    QString     mPendingVoiceChatText;
    bool        mApplicationRestartPending = false;
};

} // namespace mod
