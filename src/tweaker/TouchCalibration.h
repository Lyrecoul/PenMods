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
 * 向 /etc/udev/rules.d 写入校准矩阵，补偿屏幕边缘一个像素的死区。
 * 矩阵只在开机时被读取，因此切换后需要重启系统才生效。
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

    void rebootSystem();
};

} // namespace mod
