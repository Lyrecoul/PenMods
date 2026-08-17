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

public:
    // 为属性变更添加信号
    Q_SIGNAL void versionChanged();
    Q_SIGNAL void cachedSymCountChanged();
    Q_SIGNAL void buildInfoChanged();
    Q_SIGNAL void voiceChatEnabledChanged();
    Q_SIGNAL void pendingVoiceChatTextChanged();

    void onUiCompleted() const;

    [[nodiscard]] bool isTrustedDevice() const;

    [[nodiscard]] QString getVersionStr() const;

    [[nodiscard]] int getCachedSymCount() const;

    [[nodiscard]] QString getBuildInfoStr() const;

    Q_INVOKABLE [[nodiscard]] QString getOtherSlot() const;

    Q_INVOKABLE void changeSlot();

    Q_INVOKABLE void uninstall();

    Q_INVOKABLE void softReboot();

    Q_INVOKABLE void reboot();

    bool getVoiceChatEnabled() const;
    void setVoiceChatEnabled(bool enabled);
    QString getPendingVoiceChatText() const;
    void setPendingVoiceChatText(const QString& text);

private:
    friend Singleton<Mod>;
    explicit Mod();

    std::string mClassName = "mod";
    bool mVoiceChatEnabled = false;
    QString mPendingVoiceChatText;
};

} // namespace mod
