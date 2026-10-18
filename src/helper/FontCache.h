// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

class QProcess;

namespace mod {

/*
 * fontconfig 环境预检查。
 *
 * 公式渲染服务（mathjax-server / resvg-js）自己用 fontdb 解析 /etc/fonts/fonts.conf，不需要 fontconfig
 * 缓存；但设备上其它吃字体的东西都要：内嵌播放器的 libass 字幕、Qt 自己的 fontconfig 后端（字体 fallback
 * 顺序）、以及我们 exec 出去的 mpv/curl 等子孙进程。出厂镜像里没有缓存时，第一次 fc-list 要 **40 秒**才
 * 把 144 个字体扫完并写盘（实测 YDP02X，冷启动 39.9 s / 命中缓存 40 ms），这段时间里调用方要么卡住、要么
 * 拿不到中文字体。
 *
 * 因此开机时做两件事，按顺序：
 *
 *   1. 字体目录。fonts.conf 里只列了 <dir>/usr/share/fonts</dir>，真正的字体在 /usr/lib/fonts，
 *      而出厂镜像**没有**这条软链（测试机上那条是手动建的）。所以干净机器上 fontconfig 和
 *      resvg-js/fontdb 一个字体都找不到 —— 缺了（或指向别处、或是空目录）就补上
 *      /usr/share/fonts -> /usr/lib/fonts。设备自己往那个目录里放了东西就一律不碰。
 *   2. 字体清单缓存。/var/cache/fontconfig 里没有可用的 <hash>-<machine>.cache-<n> 时才跑 fc-cache
 *      （不加 -f，已是最新的缓存不会重扫）。冷启动要 40 秒，所以必须异步跑，不能挡 UI 线程。
 *
 * 时序：与 CaCertificates 一致，推迟到 Event::beforeUiInitialization —— 那里 QCoreApplication 已经存在，
 * QProcess 才能挂在事件循环上。
 */
class FontCache : public Singleton<FontCache>, private Logger {
public:
    // 字体目录是这次补出来的（而不是设备本来就有）。
    [[nodiscard]] bool getFontDirRepaired() const { return mFontDirRepaired; }

    // 磁盘上已经有一份可用的 fontconfig 缓存（后台 fc-cache 完成后才变 true）。
    [[nodiscard]] bool isCacheReady() const { return mCacheReady; }

    // 后台 fc-cache 还在跑。
    [[nodiscard]] bool isRebuilding() const { return mRebuilding; }

private:
    friend Singleton<FontCache>;
    FontCache();

    void _install();

    // 补 /usr/share/fonts -> /usr/lib/fonts，返回是否补成功（本来就可用也算成功）。
    bool _linkFontDir();

    // 异步跑 fc-cache。
    void _rebuildCache();

    bool mFontDirRepaired = false;
    bool mCacheReady      = false;
    bool mRebuilding      = false;

    QProcess* mProcess = nullptr;
};

} // namespace mod
