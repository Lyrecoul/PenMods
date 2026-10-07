// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "filemanager/player/ExternalPlayer.h"

#include "base/YPointer.h"

#include "filemanager/FileManager.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QQmlContext>

namespace mod::filemanager {

namespace {

// 内嵌 FFmpeg 播放器（QML 插件）部署目录，存在时由它接管视频播放，
// 不再拉起独立的 mpv（进程内播放能跟随左右手旋转、支持画面内控件与字幕）。
const char* EmbeddedPlayerModule = "/userdata/PenMods/qml/FFmpegPlayer/libffmpegplayerplugin.so";

// 左右手模式只在主程序的 QML 层做 180° 旋转, mpv 作为独立进程拿不到这个信息.
bool isLeftHandMode() {
    auto* settingManager = YPointer<YSettingManager>::getInstance();
    if (!settingManager) {
        return false;
    }
    auto getter = reinterpret_cast<bool (*)(void*)>(PEN_SYM("_ZNK15YSettingManager15isRightHandModeEv"));
    if (!getter) {
        return false;
    }
    return !getter(settingManager);
}

} // namespace

ExternalPlayer::ExternalPlayer() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("externalPlayer", this);
    });
} // namespace mod::filemanager

bool ExternalPlayer::hasEmbeddedPlayer() const { return QFile::exists(EmbeddedPlayerModule); }

void ExternalPlayer::open(const QString& path) {
    mOpeningFileName = path;
    emit mediaChanged();
    if (hasEmbeddedPlayer()) {
        return;
    }
    QStringList args;
    if (isLeftHandMode()) {
        args << "--video-rotate=180";
    }
    args << getOpeningPath();
    QProcess::startDetached(QStringLiteral("/userdisk/VideoPlayer"), args);
}

QString ExternalPlayer::getOpeningPath() {
    return "file://" + FileManager::getInstance().getCurrentPath().absoluteFilePath(mOpeningFileName);
}

QString ExternalPlayer::getTitle() { return QFileInfo(mOpeningFileName).completeBaseName(); }

QString ExternalPlayer::getSubtitlePath() {
    const QFileInfo video(FileManager::getInstance().getCurrentPath().absoluteFilePath(mOpeningFileName));
    const QDir      dir = video.absoluteDir();
    // 外挂字幕：与视频同名的 .ass / .lrc（插件支持这两种），优先级 ass > lrc
    for (const char* suffix : {".ass", ".lrc"}) {
        const QString candidate = dir.absoluteFilePath(video.completeBaseName() + suffix);
        if (QFile::exists(candidate)) return "file://" + candidate;
    }
    return {};
}
} // namespace mod::filemanager
