// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "helper/FontCache.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QQmlContext>
#include <QQuickView>
#include <QStringList>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>

#include <unistd.h>

namespace mod {

namespace {

// fonts.conf 里唯一的 <dir>；真字体在 /usr/lib/fonts（见 AGENTS.md 的 MathJax 一节）。
constexpr auto kFontDir       = "/usr/share/fonts";
constexpr auto kFontDirSource = "/usr/lib/fonts";

// fontconfig 的 cache 文件：<hash8>-<machine>.cache-<格式版本>。空目录写出来的缓存只有几百字节，
// 真扫到字体是几百 KB，所以用 1 KiB 当门槛，再校验文件头，避免把上次中断留下的残片当成"已就绪"。
constexpr qint64        kMinCacheSize  = 1024;
constexpr unsigned char kCacheMagic[4] = {0x04, 0xFC, 0x02, 0xFC};

bool hasFontSuffix(const QString& name) {
    static const QStringList kSuffixes{
        QStringLiteral(".ttf"),
        QStringLiteral(".ttc"),
        QStringLiteral(".otf"),
        QStringLiteral(".otc"),
        QStringLiteral(".pfa"),
        QStringLiteral(".pfb"),
        QStringLiteral(".pcf"),
        QStringLiteral(".pcf.gz"),
        QStringLiteral(".bdf"),
        QStringLiteral(".woff"),
        QStringLiteral(".woff2"),
    };
    const auto lower = name.toLower();
    return std::any_of(kSuffixes.cbegin(), kSuffixes.cend(), [&lower](const QString& s) { return lower.endsWith(s); });
}

// /usr/share/fonts 现在能不能用：必须解析成一个装着字体文件的目录。悬空软链、空目录、指向别处都算不能用。
// 只看 QFileInfo::exists() 分辨不出"目录在、里面没字体"，而字体服务靠的正是内容。
bool fontDirLooksUsable() {
    const QFileInfo info(QString::fromLatin1(kFontDir));
    if (!info.exists() || !info.isDir()) {
        return false;
    }
    const QDir dir(QString::fromLatin1(kFontDir));
    for (const auto& name : dir.entryList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (hasFontSuffix(name)) {
            return true;
        }
    }
    return false;
}

// fc-cache 会写到哪个目录：编译进去的是 /var/cache/fontconfig（设备上 fc-cache -v 只 clean 这一个），
// 另两个只是 $HOME 未设时 fc-cache 会退回到的位置，列上是为了不误判成"没有缓存"而白扫 40 秒。
QStringList cacheDirCandidates() {
    QStringList dirs{QStringLiteral("/var/cache/fontconfig")};
    const auto  xdg = qEnvironmentVariable("XDG_CACHE_HOME");
    if (!xdg.isEmpty()) {
        dirs << xdg + QStringLiteral("/fontconfig");
    }
    const auto home = qEnvironmentVariable("HOME");
    dirs << (home.isEmpty() ? QStringLiteral("/.cache/fontconfig") : home + QStringLiteral("/.cache/fontconfig"));
    return dirs;
}

bool looksLikeCacheFile(const QString& path) {
    const QFileInfo info(path);
    if (!info.isFile() || info.size() < kMinCacheSize) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto head = file.read(4);
    return head.size() == 4 && std::memcmp(head.constData(), kCacheMagic, sizeof(kCacheMagic)) == 0;
}

// 现有缓存的最新时间戳（0 = 一份都没有）。
qint64 newestCacheMTime() {
    qint64 newest = 0;
    for (const auto& dirPath : cacheDirCandidates()) {
        const QDir dir(dirPath);
        for (const auto& name : dir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot)) {
            if (!name.contains(QStringLiteral(".cache-"))) {
                continue;
            }
            const auto path = dir.filePath(name);
            if (!looksLikeCacheFile(path)) {
                continue;
            }
            newest = std::max(newest, QFileInfo(path).lastModified().toMSecsSinceEpoch());
        }
    }
    return newest;
}

// rootfs 反正是 rw 的（Mod::onUiCompleted 也会 remount），只有真写不进去时才重挂一次。
void remountRootReadWrite() { exec("mount -o remount,rw /"); }

bool isWritableFailure(int error) { return error == EROFS || error == EACCES || error == EPERM; }

// 删掉不可用的 /usr/share/fonts（软链走 unlink，空目录走 rmdir），写不进去时重挂 rootfs 再试一次。
bool removeUnusableTarget(bool isSymLink) {
    const auto doRemove = [isSymLink]() { return isSymLink ? ::unlink(kFontDir) == 0 : ::rmdir(kFontDir) == 0; };
    if (doRemove()) {
        return true;
    }
    if (errno == ENOENT) {
        return true;
    }
    if (!isWritableFailure(errno)) {
        return false;
    }
    remountRootReadWrite();
    return doRemove() || errno == ENOENT;
}

bool createFontDirLink() {
    if (::symlink(kFontDirSource, kFontDir) == 0) {
        return true;
    }
    if (!isWritableFailure(errno)) {
        return false;
    }
    remountRootReadWrite();
    return ::symlink(kFontDirSource, kFontDir) == 0;
}

} // namespace

FontCache::FontCache() : Logger("FontCache") {
    QObject::connect(&Event::getInstance(), &Event::beforeUiInitialization, [this]() { _install(); });
}

void FontCache::_install() {
    static std::once_flag once;
    std::call_once(once, [this] {
        if (!fontDirLooksUsable()) {
            mFontDirRepaired = _linkFontDir();
        }
        if (!fontDirLooksUsable()) {
            // 目录不可用时宁可什么都不做：扫一个空目录只会写出一份"没有字体"的缓存，
            // 之后 fontconfig 还会拿它当有效缓存用。
            warn("{} is unusable, skipping fontconfig cache build", kFontDir);
            return;
        }

        const auto   newest     = newestCacheMTime();
        const qint64 fontsMTime = QFileInfo(QString::fromLatin1(kFontDir)).lastModified().toMSecsSinceEpoch();
        mCacheReady             = newest > 0;
        const bool stale        = mCacheReady && fontsMTime > newest;

        if (mCacheReady && !stale && !mFontDirRepaired) {
            info("fontconfig cache is ready, nothing to do");
            return;
        }

        if (qEnvironmentVariableIsSet("PENMODS_NO_FONT_CACHE")) {
            warn(
                "PENMODS_NO_FONT_CACHE is set, not running fc-cache ({} / {})",
                mCacheReady ? "cache present" : "no cache",
                stale ? "cache stale" : "cache fresh"
            );
            return;
        }

        if (mFontDirRepaired) {
            warn("Linked {} -> {}, building the fontconfig cache", kFontDir, kFontDirSource);
        } else if (!mCacheReady) {
            warn("No fontconfig cache found, running fc-cache (the first scan takes ~40 s on this CPU)");
        } else {
            warn("Fonts are newer than the fontconfig cache, refreshing it with fc-cache");
        }
        _rebuildCache();
    });
}

bool FontCache::_linkFontDir() {
    if (!QFileInfo(QString::fromLatin1(kFontDirSource)).isDir()) {
        error("{} is missing, cannot provide {}", kFontDirSource, kFontDir);
        return false;
    }

    const QFileInfo link(QString::fromLatin1(kFontDir));
    const bool      isSymLink = link.isSymLink();
    if (isSymLink || link.exists()) {
        if (!isSymLink) {
            if (!link.isDir()) {
                error("{} exists and is not a directory, leaving it alone", kFontDir);
                return false;
            }
            if (!QDir(QString::fromLatin1(kFontDir)).isEmpty()) {
                // 设备自己往这个目录里放了东西（哪怕是别的布局）：不碰用户/厂商的数据。
                error("{} holds no usable font but is not empty, leaving it alone", kFontDir);
                return false;
            }
        }
        if (!removeUnusableTarget(isSymLink)) {
            error("Cannot remove {} ({})", kFontDir, std::strerror(errno));
            return false;
        }
    }

    const QDir shareDir = link.dir();
    if (!shareDir.exists() && !QDir().mkpath(shareDir.absolutePath())) {
        error("Cannot create {}", shareDir.absolutePath().toStdString());
        return false;
    }

    if (!createFontDirLink()) {
        error("Cannot link {} -> {} ({})", kFontDir, kFontDirSource, std::strerror(errno));
        return false;
    }
    if (!fontDirLooksUsable()) {
        error("{} still holds no font after linking", kFontDir);
        return false;
    }
    return true;
}

void FontCache::_rebuildCache() {
    if (mProcess != nullptr) {
        return;
    }

    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();

    mProcess = new QProcess();
    // 冷启动输出很少，合并后一次读完，省一个槽。
    mProcess->setProcessChannelMode(QProcess::MergedChannels);

    QObject::connect(
        mProcess,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        mProcess,
        [this, elapsed](int code, QProcess::ExitStatus status) {
            const auto output = QString::fromUtf8(mProcess->readAll()).trimmed();
            mRebuilding       = false;
            if (status == QProcess::NormalExit && code == 0) {
                mCacheReady = newestCacheMTime() > 0;
                if (mCacheReady) {
                    info("fc-cache finished in {} ms, fontconfig cache is ready", elapsed->elapsed());
                } else {
                    error("fc-cache exited 0 but no usable cache appeared in /var/cache/fontconfig");
                }
            } else {
                error(
                    "fc-cache failed (exit {}, {}, {} ms), output: {}",
                    code,
                    status == QProcess::CrashExit ? "crashed" : "normal exit",
                    elapsed->elapsed(),
                    output.isEmpty() ? "<empty>" : output.toStdString()
                );
            }
            mProcess->deleteLater();
            mProcess = nullptr;
        }
    );

    // FailedToStart 不会再发 finished，得在这里收尾；其它错误交给 finished 统一报。
    QObject::connect(mProcess, &QProcess::errorOccurred, mProcess, [this](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) {
            return;
        }
        mRebuilding = false;
        error("Cannot start fc-cache: {}", mProcess->errorString().toStdString());
        mProcess->deleteLater();
        mProcess = nullptr;
    });

    mRebuilding = true;
    // 不带 -f：已经是最新的目录不会重扫，命中时只要几十毫秒。
    mProcess->start(QStringLiteral("fc-cache"), QStringList());
}

} // namespace mod
