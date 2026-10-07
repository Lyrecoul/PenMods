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

// 触摸 IC 上报的 ABS_MT_POSITION_X/Y 量程是 0..170 / 0..320（`evtest` 可查），而面板正好是
// 170x320 像素：最外侧那一列的原始坐标归一化后会落到屏幕外的那一像素上，QML 收不到按下事件，
// 表现为"从屏幕边缘向内滑动没有反应"。+1/320 就是把上报坐标往屏幕内挪一个像素，让边缘触点
// 落在最后一列上。矩阵语义见 libinput(4) 的 LIBINPUT_CALIBRATION_MATRIX。
constexpr auto RULE_MATRIX = "1 0 0 0 1 0.003125";

// hyn_ts：本机（YDP02X）实测的 udev 名字；ft3427_ts：另一台 YDP02X 上出现过的名字。
// 新增机型时在这里补名字即可（YDPG3/YDP03X 的触摸 IC 名字尚未确认，规则不会匹配到它们）。
constexpr auto RULE_DEVICES = "hyn_ts|ft3427_ts";

QByteArray ruleContent() {
    return QByteArray("ATTRS{name}==\"") + RULE_DEVICES + "\", ENV{LIBINPUT_CALIBRATION_MATRIX}=\"" + RULE_MATRIX
         + "\"\n";
}

} // namespace

TouchCalibration::TouchCalibration() : Logger("TouchCalibration") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("touchCalibration", this);
    });
}

bool TouchCalibration::enabled() const { return QFile::exists(RULE_PATH); }

void TouchCalibration::setEnabled(bool value) {
    if (value == enabled()) return;

    if (value) {
        if (!installRule()) return; // installRule 里已经提示过失败原因
        info("Touch calibration enabled; takes effect after a restart.");
    } else if (!QFile::remove(RULE_PATH)) {
        fail(QString("关闭触摸校准失败：无法删除 %1").arg(RULE_PATH));
        return;
    } else {
        info("Touch calibration disabled; takes effect after a restart.");
    }

    emit stateChanged();
    rebootSystem();
}

// 先写临时文件再 rename，避免中途断电留下半条规则
bool TouchCalibration::installRule() {
    if (!QDir().mkpath(RULE_DIR)) {
        fail(QString("触摸校准失败：无法创建 %1").arg(RULE_DIR));
        return false;
    }

    const QByteArray content = ruleContent();
    const QString    temp    = QString(RULE_PATH) + ".new";

    QFile::remove(temp);
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(content) != static_cast<qint64>(content.size())) {
        file.close();
        QFile::remove(temp);
        fail(QString("触摸校准失败：无法写入 %1").arg(RULE_DIR));
        return false;
    }
    file.close();

    QFile::remove(RULE_PATH);
    if (!QFile::rename(temp, RULE_PATH)) {
        QFile::remove(temp);
        fail(QString("触摸校准失败：无法写入 %1").arg(RULE_DIR));
        return false;
    }
    return true;
}

void TouchCalibration::fail(const QString& message) {
    warn("{}", message.toStdString());
    showToast(message.toStdString(), "#E9900C");
    // 让 QML 开关回到磁盘上的真实状态（写入失败时它已经被用户拨到另一侧了）
    emit stateChanged();
}

void TouchCalibration::rebootSystem() {
    // udevd 只在开机时解析规则、libinput 只在设备打开时读取该属性，所以只能重启整机；
    // reboot 会杀掉本进程，因此放到后台子 shell 里延迟一秒执行，先让 UI 与日志收尾。
    exec("sync; (sleep 1; reboot) >/dev/null 2>&1 &");
}

} // namespace mod
