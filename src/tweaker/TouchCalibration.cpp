// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/TouchCalibration.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QDir>
#include <QFile>
#include <QQmlContext>

namespace mod {

namespace {

constexpr auto RULE_DIR  = "/etc/udev/rules.d";
constexpr auto RULE_PATH = "/etc/udev/rules.d/99-penmods-touch-calibration.rules";

// 1/320 即一个像素
constexpr auto RULE_CONTENT = "ATTRS{name}==\"ft3427_ts\", ENV{LIBINPUT_CALIBRATION_MATRIX}=\"1 0 0 0 1 0.003125\"\n";

} // namespace

TouchCalibration::TouchCalibration() : Logger("TouchCalibration") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("touchCalibration", this);
    });
}

bool TouchCalibration::enabled() const { return QFile::exists(RULE_PATH); }

void TouchCalibration::setEnabled(bool value) {
    if (value == enabled()) {
        return;
    }

    if (value) {
        if (!QDir().mkpath(RULE_DIR)) {
            warn("Cannot create {}; touch calibration stays off.", RULE_DIR);
            return;
        }
        const QString temp = QString(RULE_PATH) + ".new";
        QFile         file(temp);
        QFile::remove(temp);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(RULE_CONTENT) < 0) {
            warn("Cannot write {} (read-only root?); touch calibration stays off.", temp.toStdString());
            return;
        }
        file.close();
        QFile::remove(RULE_PATH);
        if (!QFile::rename(temp, RULE_PATH)) {
            QFile::remove(temp);
            warn("Cannot move the rule into {}; touch calibration stays off.", RULE_PATH);
            return;
        }
        info("Touch calibration enabled; takes effect after a restart.");
    } else {
        if (!QFile::remove(RULE_PATH)) {
            warn("Cannot remove {}; touch calibration stays on.", RULE_PATH);
            return;
        }
        info("Touch calibration disabled; takes effect after a restart.");
    }

    emit stateChanged();
    rebootSystem();
}

void TouchCalibration::rebootSystem() {
    // 校准矩阵只在开机时读取，先 sync 再把 app 拉起来
    exec("sync; (sleep 1; reboot) >/dev/null 2>&1 &");
}

} // namespace mod
