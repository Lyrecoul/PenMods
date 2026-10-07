// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

namespace mod::filemanager {

class ExternalPlayer : public QObject, public Singleton<ExternalPlayer> {
    Q_OBJECT

    Q_PROPERTY(QString path READ getOpeningPath NOTIFY mediaChanged);
    Q_PROPERTY(QString title READ getTitle NOTIFY mediaChanged);
    Q_PROPERTY(QString subtitlePath READ getSubtitlePath NOTIFY mediaChanged);

    /// 内嵌播放器（QML 插件）是否已部署。QML 页面据此决定加载哪套界面：
    /// 没有插件时退回"拉起 mpv + 占位页"的老行为。
    Q_PROPERTY(bool hasEmbeddedPlayer READ hasEmbeddedPlayer CONSTANT)

public:
    Q_INVOKABLE void open(const QString &path);

    QString getOpeningPath();

    /// 供播放页显示的标题（不含扩展名的文件名）
    QString getTitle();

    /// 同目录同名的 .ass/.lrc 字幕（file:// URL），没有则返回空串
    QString getSubtitlePath();

    [[nodiscard]] bool hasEmbeddedPlayer() const;

signals:

    void mediaChanged();

private:
    friend Singleton<ExternalPlayer>;
    explicit ExternalPlayer();

    QString mOpeningFileName;
};

}