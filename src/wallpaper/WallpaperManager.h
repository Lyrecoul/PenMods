// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Singleton.h"

#include <QObject>
#include <QStringList>
#include <QTimer>

namespace mod {

class WallpaperManager : public QObject, public Singleton<WallpaperManager> {
    Q_OBJECT

    // 0=无壁纸(纯色背景), 1=单张自定义, 2=随机循环
    Q_PROPERTY(int wallpaperMode READ getWallpaperMode WRITE setWallpaperMode NOTIFY wallpaperModeChanged);

    // 单张模式下的图片路径
    Q_PROPERTY(QString customImagePath READ getCustomImagePath WRITE setCustomImagePath NOTIFY customImagePathChanged);

    // 随机循环模式的文件夹路径
    Q_PROPERTY(QString wallpaperFolder READ getWallpaperFolder WRITE setWallpaperFolder NOTIFY wallpaperFolderChanged);

    // 循环切换间隔(秒)
    Q_PROPERTY(int cycleInterval READ getCycleInterval WRITE setCycleInterval NOTIFY cycleIntervalChanged);

    // 当前壁纸路径（只读）
    Q_PROPERTY(QString currentWallpaper READ getCurrentWallpaper NOTIFY currentWallpaperChanged);

public:
    explicit WallpaperManager(QObject* parent = nullptr);

    int getWallpaperMode() const;
    void setWallpaperMode(int mode);

    QString getCustomImagePath() const;
    void setCustomImagePath(const QString& path);

    QString getWallpaperFolder() const;
    void setWallpaperFolder(const QString& path);

    int getCycleInterval() const;
    void setCycleInterval(int seconds);

    QString getCurrentWallpaper() const;

    // 扫描指定目录下的图片文件，返回文件路径列表
    Q_INVOKABLE QStringList scanFolder(const QString& folderPath);

    // 扫描当前配置的壁纸文件夹
    Q_INVOKABLE QStringList scanWallpaperFolder();

    // 设置当前壁纸
    Q_INVOKABLE void setWallpaper(const QString& path);

    // 切换到下一张（循环模式用）
    Q_INVOKABLE void nextWallpaper();

signals:
    void wallpaperModeChanged();
    void customImagePathChanged();
    void wallpaperFolderChanged();
    void cycleIntervalChanged();
    void currentWallpaperChanged();

    // 文件夹扫描完成，返回文件列表
    void wallpaperFolderUpdated(const QStringList& files);

private slots:
    void onCycleTimerTick();

private:
    void saveConfig();
    void loadConfig();
    void startCycleTimer();
    void stopCycleTimer();
    void applyWallpaper(const QString& path);
    void updateAvailability();

    int     mWallpaperMode = 0;       // 默认无壁纸
    QString mCustomImagePath;          // 单张模式路径
    QString mWallpaperFolder;          // 循环模式文件夹
    int     mCycleInterval = 300;      // 默认 5 分钟
    QString mCurrentWallpaper;         // 当前壁纸

    QStringList mCachedImages;         // 缓存的图片列表
    int         mCurrentIndex = -1;    // 当前图片在列表中的索引

    QTimer* mCycleTimer = nullptr;

    friend class Singleton<WallpaperManager>;
};

} // namespace mod