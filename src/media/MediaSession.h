// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

#include <QTimer>

struct PluginMediaAPI;

namespace mod {

/**
 * @brief 插件媒体会话
 *
 * 让插件把自己播放的内容暴露给系统的音乐控制 UI（下拉快捷设置面板）。
 * 同一时刻只允许一个插件持有会话；没有插件会话时面板回落到宿主播放器
 * (mediaPlayerManager)，所以宿主原有的行为不受影响。
 *
 * QML 插件用法见 doc/PLUGIN_DEV_GUIDE.md，C 插件用法见 PluginSDK.h。
 */
class MediaSession : public QObject, public Singleton<MediaSession>, private Logger {
    Q_OBJECT

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString ownerPluginId READ ownerPluginId NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(QString artist READ artist WRITE setArtist NOTIFY artistChanged)
    Q_PROPERTY(QString album READ album WRITE setAlbum NOTIFY albumChanged)
    Q_PROPERTY(QString cover READ cover WRITE setCover NOTIFY coverChanged)
    Q_PROPERTY(int duration READ duration WRITE setDuration NOTIFY durationChanged)
    Q_PROPERTY(int position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(PlayState playState READ playState WRITE setPlayState NOTIFY playStateChanged)
    Q_PROPERTY(bool hasLyrics READ hasLyrics NOTIFY lyricsChanged)
    Q_PROPERTY(QString mainLyric READ mainLyric WRITE setMainLyric NOTIFY lyricsChanged)
    Q_PROPERTY(QString transLyric READ transLyric WRITE setTransLyric NOTIFY lyricsChanged)

public:
    /// 与 PluginSDK.h 的 PluginMediaPlayState 保持一致
    enum PlayState { Stopped = 0, Playing = 1, Paused = 2 };
    Q_ENUM(PlayState)

    /// 声明一个插件会话；同一插件重复调用是幂等的。失败（id 为空）返回 false。
    Q_INVOKABLE bool begin(const QString& pluginId);

    /// 释放当前会话，面板回落到宿主播放器。
    Q_INVOKABLE void end();

    /// 一次性设置歌词；传空串表示没有歌词。
    Q_INVOKABLE void setLyrics(const QString& mainLyric, const QString& transLyric);

    /// 插件被禁用/卸载时由 PluginManager 调用，避免会话悬挂。
    void releaseFor(const QString& pluginId);

    /// 递给 init_plugin_with_media_api 的 C ABI 结构体（生命周期与进程一致）。
    static PluginMediaAPI* pluginApi();

    bool      active() const { return mActive; }
    QString   ownerPluginId() const { return mOwnerPluginId; }
    QString   title() const { return mTitle; }
    QString   artist() const { return mArtist; }
    QString   album() const { return mAlbum; }
    QString   cover() const { return mCover; }
    int       duration() const { return mDuration; }
    int       position() const { return mPosition; }
    PlayState playState() const { return mPlayState; }
    bool      hasLyrics() const { return !mMainLyric.isEmpty() || !mTransLyric.isEmpty(); }
    QString   mainLyric() const { return mMainLyric; }
    QString   transLyric() const { return mTransLyric; }

    void setTitle(const QString& value);
    void setArtist(const QString& value);
    void setAlbum(const QString& value);
    void setCover(const QString& value);
    void setDuration(int value);
    void setPosition(int value);
    void setPlayState(PlayState value);
    void setMainLyric(const QString& value);
    void setTransLyric(const QString& value);

signals:
    void activeChanged();
    void titleChanged();
    void artistChanged();
    void albumChanged();
    void coverChanged();
    void durationChanged();
    void positionChanged();
    void playStateChanged();
    void lyricsChanged();

    // 面板 -> 插件 的控制事件
    void playRequested();
    void pauseRequested();
    void toggleRequested();
    void nextRequested();
    void prevRequested();
    void stopRequested();
    void seekRequested(int position);
    void openRequested();

    // 会话被别的插件接管，旧持有者应停止播放并 end()
    void sessionRevoked();

private:
    friend Singleton<MediaSession>;
    explicit MediaSession();

    void reset();
    void syncPositionTimer();

    bool    mActive{false};
    QString mOwnerPluginId;

    QString   mTitle;
    QString   mArtist;
    QString   mAlbum;
    QString   mCover;
    int       mDuration{0};
    int       mPosition{0};
    PlayState mPlayState{Stopped};
    QString   mMainLyric;
    QString   mTransLyric;

    QTimer mPositionTimer;
};

} // namespace mod
