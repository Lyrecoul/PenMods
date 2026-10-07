// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

namespace mod {

/**
 * @brief 触摸校准开关
 *
 * 往 /etc/udev/rules.d 写一条 LIBINPUT_CALIBRATION_MATRIX 规则，补偿触摸 IC 量程比面板
 * 多一个像素造成的边缘死区（矩阵怎么来的见 .cpp 里的注释）。
 *
 * udevd 只在开机时解析规则、libinput 只在设备打开时读取该属性，因此切换后需要重启整机。
 * 写入失败时会弹出提示并让开关回到磁盘上的真实状态。
 */
class TouchCalibration : public QObject, public Singleton<TouchCalibration>, private Logger {
    Q_OBJECT

    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged)

public:
    /// 校准规则是否存在
    [[nodiscard]] bool enabled() const;

    void setEnabled(bool value);

signals:

    void stateChanged();

private:
    friend Singleton<TouchCalibration>;
    explicit TouchCalibration();

    bool installRule();
    void fail(const QString& message);
    void rebootSystem();
};

} // namespace mod
