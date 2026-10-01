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

#include <QProcess>
#include <QQmlContext>

namespace mod::filemanager {

namespace {

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

void ExternalPlayer::open(const QString& path) {
    mOpeningFileName = path;
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
} // namespace mod::filemanager
