// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

namespace mod {

/**
 * @brief 实验性开关：给设备树的 VPU 节点解除禁用（硬件视频解码）
 *
 * 出厂固件的设备树把 `vpu_combo` 标成 `disabled`，所以内核不绑定 rk-vcodec 驱动、
 * 不生成 `/dev/vpu_service`，任何走 Rockchip MPP 的硬解都起不来（而且厂商 MPP 用户态
 * 在没有 VPU 时会在 `mpp_init()` 里段错误，见 doc/FFMPEG_PLAYER_ANALYSIS.md §2.4）。
 *
 * 内核本身带 rk-vcodec 驱动，缺的只是 DTB 里的那一个属性 —— 而 DTB 就在 boot 分区的
 * 镜像里（`boot_a`/`boot_b`），所以这里直接：
 *
 *  1. 把当前 slot 的出厂 boot 镜像备份到 `/userdisk/PenMods/boot_stock.img`（只备份一次）；
 *  2. 复制一份并就地改写 DTB 里 `vpu_combo/status`（`disabled`/`okey` → `okay`，只动属性和
 *     长度字段，不需要重新编译设备树），保存为 `/userdisk/PenMods/boot_vpu.img`；
 *  3. 写回 boot 分区、回读校验；任何一步失败都立刻把出厂镜像刷回去；
 *  4. 重启整机（设备树只在启动时被解析）。
 *
 * 关掉开关就是把出厂镜像刷回去再重启。
 */
class VpuUnlock : public QObject, public Singleton<VpuUnlock>, private Logger {
    Q_OBJECT

    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)

public:
    /// 运行中的设备树里 VPU 节点是否已启用
    [[nodiscard]] bool enabled() const;

    /// 本机是否可以打补丁（需要 root、可写的 boot 分区、且存在 vpu_combo 节点）
    [[nodiscard]] bool supported();

    void setEnabled(bool value);

signals:

    void stateChanged();

private:
    friend Singleton<VpuUnlock>;
    explicit VpuUnlock();

    /// 当前 slot 的 boot 分区设备节点
    [[nodiscard]] QString bootPartition() const;

    /// 把当前 boot 分区内容备份成出厂镜像（只做一次）
    bool backupFactoryImage(QString& error);

    /// 生成已打补丁的 boot 镜像（失败时返回 false 并带出原因）
    bool makePatchedImage(QString& error);

    /// 把镜像写进 boot 分区并回读校验
    bool flashImage(const QString& image, QString& error) const;

    /// 扫描镜像里所有 FDT，把 vpu_combo/status 改成 okay；返回改动的属性个数，-1 表示镜像布局异常
    static int patchDtbs(QByteArray& image);

    void fail(const QString& message);
    void rebootSystem();
};

} // namespace mod
