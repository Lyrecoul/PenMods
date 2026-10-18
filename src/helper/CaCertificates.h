// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

namespace mod {

/*
 * 出厂 rootfs 的 /etc/ssl/certs 是空目录（GitHub issue #15），于是所有做 TLS 校验的程序都静默失败，
 * 只报 "unable to get local issuer certificate"。curl、内嵌 FFmpeg 播放器（静态 OpenSSL 3）和 Qt 网络栈
 * （OTA 更新、chatbot）都只认这个路径。
 *
 * 内嵌的 Mozilla CA 包（resource/certs/ca-bundle.pem，配置期由 xmake 生成为 build/config/CaBundleData.h）
 * 有两条落地路径，按顺序尝试：
 *
 *   1. bind mount（首选，整机可用）：把包拆成 CApath 需要的 <subject_hash>.0 文件写进 tmpfs，
 *      再 bind mount 到 /etc/ssl/certs。OpenSSL 的 CApath 只认这种文件名，直接丢一份 PEM 进去无效。
 *      这样本进程 Qt、内嵌静态 OpenSSL 3 的 ffmpeg 插件、以及我们 exec 出去的 curl/mpv 都能用，
 *      且不写 rootfs（/etc/ssl/certs 只是被遮住，重启即失效）。
 *   2. 进程内默认信任锚（兜底）：mount 失败（无 CAP_SYS_ADMIN、namespace 私有、/etc/ssl/certs 不存在…）
 *      时退回 QSslConfiguration::setDefaultConfiguration()，只修本进程的 Qt 网络栈。
 *
 * 设备自己已有证书（未来固件，或用户按 issue #15 装过）时两条路都不走。
 *
 * 时序：工作不能放在 BeforeMain —— 那时还没有 QCoreApplication，QtNetwork 的 TLS 后端初始化会拿不到
 * 插件/库搜索路径，并把失败结果缓存下来。因此推迟到 Event::beforeUiInitialization。
 */
class CaCertificates : public Singleton<CaCertificates>, private Logger {
public:
    // 走 bind mount 时 /etc/ssl/certs 里发布的证书数量（0 = 没走 mount）。
    [[nodiscard]] int getMountedCount() const { return mMountedCount; }

    // 退回进程内默认信任锚时注入的证书数量（0 = 没走兜底）。
    [[nodiscard]] int getInjectedCount() const { return mInjectedCount; }

private:
    friend Singleton<CaCertificates>;
    CaCertificates();

    void _install();

    int mMountedCount  = 0;
    int mInjectedCount = 0;
};

} // namespace mod
