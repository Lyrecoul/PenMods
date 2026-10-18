// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "helper/CaCertificates.h"

#include "common/Event.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSslCertificate>
#include <QSslConfiguration>

#include <QQmlContext>
#include <QQuickView>

#include <cerrno>
#include <cstring>
#include <mutex>

#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "CaBundleData.h"

namespace mod {

namespace {

// 设备上的 CApath 目录：OpenSSL 默认就会读它（curl 7.58 / 内嵌 OpenSSL 3 的 ffmpeg 插件 / Qt 都一样）。
constexpr auto kMountTarget = "/etc/ssl/certs";
constexpr auto kMarkerName  = ".penmods-ca";

// 发布目录（bind mount 的源）。优先 tmpfs：重启即消失，不占用户分区；/tmp 不可写时退到 /userdata。
constexpr const char* kStagingCandidates[] = {
    "/tmp/penmods-ca",
    "/userdata/PenMods/ca-staging",
};

QByteArray bundlePem() {
    return QByteArray::fromRawData(certs::kCaBundlePem, static_cast<int>(sizeof(certs::kCaBundlePem) - 1));
}

// 拆成单张证书块：CApath 的每个 <hash>.0 只放一张。若每个 hash 文件都塞整包，
// 121 份 182 KiB 会在 tmpfs 里翻成 22 MiB。
QList<QByteArray> splitPem(const QByteArray& pem) {
    static const QByteArray kBegin("-----BEGIN CERTIFICATE-----");
    static const QByteArray kEnd("-----END CERTIFICATE-----");

    QList<QByteArray> blocks;
    int               pos = 0;
    while (true) {
        const int begin = pem.indexOf(kBegin, pos);
        if (begin < 0) {
            break;
        }
        const int end = pem.indexOf(kEnd, begin);
        if (end < 0) {
            break;
        }
        const int  stop  = end + kEnd.size();
        QByteArray block = pem.mid(begin, stop - begin);
        if (!block.endsWith('\n')) {
            block.append('\n');
        }
        blocks.append(block);
        pos = stop;
    }
    return blocks;
}

// CApath 的文件名：<8 位小写十六进制 subject hash>.<序号>。
bool isCaLookupName(const QString& name) {
    if (name.size() < 10 || name.at(8) != QLatin1Char('.')) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        const auto c = name.at(i);
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    for (int i = 9; i < name.size(); ++i) {
        if (name.at(i) < QLatin1Char('0') || name.at(i) > QLatin1Char('9')) {
            return false;
        }
    }
    return true;
}

// 这个条目里真的是一张证书吗（而不是一个被 cp/打包工具"物化"成文本的软链占位）。
// 实测过一台机器：/etc/ssl/certs 有 438 个条目，其中 292 个 <hash>.0 每个只有 20~38 字节，
// 内容是 "GlobalSign_Root_R46.pem" 这样的文件名 —— 看着很满，OpenSSL 一张都读不出来。
bool looksLikeCertificateFile(const QString& path) {
    const QFileInfo info(path);
    if (!info.isFile() || info.size() < 100) { // 最小的根证书也有 400+ 字节
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto head = file.read(64).trimmed();
    if (head.startsWith(QByteArrayLiteral("-----BEGIN CERTIFICATE-----"))
        || head.startsWith(QByteArrayLiteral("-----BEGIN TRUSTED CERTIFICATE-----"))) {
        return true;
    }
    return !head.isEmpty() && static_cast<unsigned char>(head.at(0)) == 0x30; // DER SEQUENCE
}

// 设备自己是否已经有**可用**的信任锚。两点都不能省：
//
//   - 不能用 QSslConfiguration::systemCaCertificates() 判断：Qt 5.15 的枚举结果和握手时真正使用的
//     store 不是一回事（宿主上把 SSL_CERT_DIR 指到只有 3 张证书的目录，枚举照样返回 242 个；
//     而握手确实按环境变量走），只有握手路径可信，所以直接看文件系统。
//   - 也不能只看"目录里有东西"（issue #15 的 `ls -1 /etc/ssl/certs | wc -l` 就是这么误判的）：
//     必须至少有一个 OpenSSL 真能加载的 <hash>.<n> 文件，否则等于没有。
bool deviceTrustStoreLooksUsable() {
    // 安装流程按 issue #15 修的另一种形态：只写了默认证书文件。
    if (looksLikeCertificateFile(QStringLiteral("/etc/ssl/cert.pem"))) {
        return true;
    }
    const QDir dir(QString::fromLatin1(kMountTarget));
    for (const auto& name : dir.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        if (isCaLookupName(name) && looksLikeCertificateFile(dir.filePath(name))) {
            return true;
        }
    }
    return false;
}

// 目标目录里 OpenSSL 真能加载的证书数量（挂载成功后用它验收）。
int publishedCertificateCount() {
    const QDir dir(QString::fromLatin1(kMountTarget));
    int        count = 0;
    for (const auto& name : dir.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        if (isCaLookupName(name) && looksLikeCertificateFile(dir.filePath(name))) {
            ++count;
        }
    }
    return count;
}

bool isMounted(const char* target) {
    QFile mountinfo(QStringLiteral("/proc/self/mountinfo"));
    if (!mountinfo.open(QIODevice::ReadOnly)) {
        return false;
    }
    // 不能用 atEnd()/readLine() 循环：procfs 文件的 st_size 是 0，QFile::atEnd() 会立刻返回 true，
    // 循环一句都不执行（这个坑让"复用上一个进程挂好的挂载"静默失效）。
    const auto content = mountinfo.readAll();
    for (const auto& line : content.split('\n')) {
        // 第 5 个字段是挂载点；我们的路径里没有需要转义的空格。
        const auto fields = line.split(' ');
        if (fields.size() > 4 && fields[4] == target) {
            return true;
        }
    }
    return false;
}

// 清空目录内容但**不删除目录本身**：目录可能就是当前 bind mount 的源，
// 删掉它会让挂载点指向一个已经 unlink 的 inode（看着还在，内容永远不再更新）。
bool clearDirectory(const QDir& dir) {
    for (const auto& name : dir.entryList(QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot)) {
        const auto      path = dir.filePath(name);
        const QFileInfo info(path);
        const bool removed = (info.isDir() && !info.isSymLink()) ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!removed) {
            return false;
        }
    }
    return true;
}

// 把内嵌包写成 CApath 目录（<hash>.0），返回发布目录；失败返回空。
QString publishStagingDir(const QByteArray& pem, spdlog::logger& log) {
    const auto blocks = splitPem(pem);
    if (blocks.size() != certs::kCaBundleCount) {
        log.error("Embedded CA bundle splits into {} block(s), expected {}", blocks.size(), certs::kCaBundleCount);
        return {};
    }

    // 用内容摘要做 marker：换版本后就地重写目录内容即可 —— 挂的是目录，新证书立刻透过挂载点生效。
    const auto digest = QCryptographicHash::hash(pem, QCryptographicHash::Sha256).toHex().left(16);

    for (const auto* candidate : kStagingCandidates) {
        QDir dir(QString::fromLatin1(candidate));

        QFile marker(dir.filePath(QString::fromLatin1(kMarkerName)));
        bool  upToDate = false;
        if (marker.open(QIODevice::ReadOnly)) {
            upToDate = marker.readAll().trimmed() == digest;
        }
        marker.close();
        if (upToDate) {
            // marker 命中还要确认文件数对得上，避免上次半途失败留下的残局被当成"已就绪"。
            upToDate = dir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size() == blocks.size() + 1;
        }

        if (!upToDate) {
            if (dir.exists() ? !clearDirectory(dir) : !QDir().mkpath(dir.absolutePath())) {
                log.error("Cannot prepare CA staging directory {}", candidate);
                continue;
            }
            for (int i = 0; i < blocks.size(); ++i) {
                QFile cert(dir.filePath(QString::fromLatin1(certs::kCaBundleHashes[i]) + QStringLiteral(".0")));
                if (!cert.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    || cert.write(blocks[i]) != blocks[i].size()) {
                    log.error("Cannot write {} ({})", cert.fileName().toStdString(), cert.errorString().toStdString());
                    return {};
                }
                const auto name = cert.fileName();
                cert.close();
                ::chmod(name.toUtf8().constData(), 0644);
            }
            if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                log.error(
                    "Cannot write CA staging marker {} ({})",
                    marker.fileName().toStdString(),
                    marker.errorString().toStdString()
                );
                return {};
            }
            marker.write(digest);
            marker.close();
        }

        return dir.absolutePath();
    }

    return {};
}

bool mountOverSystemStore(const QString& stagingDir, spdlog::logger& log) {
    if (!QFileInfo(QString::fromLatin1(kMountTarget)).isDir()) {
        log.error("{} is missing, cannot bind-mount the CA directory over it", kMountTarget);
        return false;
    }
    if (::mount(stagingDir.toUtf8().constData(), kMountTarget, nullptr, static_cast<unsigned long>(MS_BIND), nullptr)
        != 0) {
        log.error("mount({} -> {}) failed: {}", stagingDir.toStdString(), kMountTarget, std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace

CaCertificates::CaCertificates() : Logger("CaCertificates") {
    QObject::connect(&Event::getInstance(), &Event::beforeUiInitialization, [this]() { _install(); });
}

void CaCertificates::_install() {
    // beforeUiInitialization 只会发一次，once_flag 只是保险（重复 mount 会叠加挂载层）。
    static std::once_flag once;
    std::call_once(once, [this] {
        const auto pem              = bundlePem();
        const bool mountedByPastRun = isMounted(kMountTarget);
        const bool deviceUsable     = deviceTrustStoreLooksUsable();

        // 设备自己就有一份可用、且不是我们挂的信任锚：什么都不做。
        if (deviceUsable && !mountedByPastRun) {
            info("{} already provides usable CA certificate(s), keeping the device trust store", kMountTarget);
            return;
        }

        if (!qEnvironmentVariableIsSet("PENMODS_NO_CA_MOUNT")) {
            // 发布目录先刷新。挂的是目录，所以换 .so 后只需重写里面的文件，既不用、也不能重新 mount。
            const auto staging = publishStagingDir(pem, *this);
            const bool mounted = !staging.isEmpty() && (mountedByPastRun || mountOverSystemStore(staging, *this));
            if (mounted) {
                const auto published = publishedCertificateCount();
                if (published >= certs::kCaBundleCount) {
                    mMountedCount = published;
                    if (mountedByPastRun) {
                        info(
                            "{} is still bind-mounted by a previous run, refreshed {} certificate(s)",
                            kMountTarget,
                            published
                        );
                    } else {
                        warn(
                            "Device CA store is unusable, bind-mounted {} certificate(s) from {} over {}. See issue "
                            "#15.",
                            published,
                            staging.toStdString(),
                            kMountTarget
                        );
                    }
                    return;
                }
                error("Bind mount is in place but {} exposes only {} usable certificate(s)", kMountTarget, published);
            }
        }

        if (deviceUsable || mountedByPastRun) {
            // 我们挂的那份还在（只是这次没能刷新），或者设备自己那份可用：都别去顶掉它。
            info("Keeping the existing CA store at {}", kMountTarget);
            return;
        }

        // 兜底：只给本进程的 Qt 网络栈装默认信任锚（OTA 更新、chatbot 至少不会因为证书而全灭）。
        const auto bundled = QSslCertificate::fromData(pem, QSsl::Pem);
        if (bundled.isEmpty()) {
            error("Bundled CA bundle is unusable ({} bytes)", sizeof(certs::kCaBundlePem) - 1);
            return;
        }

        // 注意：给 caCertificates 赋值是**替换**而不是追加 —— Qt 会拿这份列表自己建 X509_STORE，
        // 系统默认路径就不再参与校验（实测：只塞一张无关证书时握手必然失败）。兜底路径只在设备
        // 完全没有可用信任锚时走到这里，所以不会顶掉设备自己的 CA。
        auto patched = QSslConfiguration::defaultConfiguration();
        patched.setCaCertificates(bundled);
        // setDefaultConfiguration 既覆盖 QNetworkAccessManager（QNetworkRequest 未显式设
        // sslConfiguration 时取它），也覆盖 QSslSocket::connectToHostEncrypted(host, port) 直连。
        QSslConfiguration::setDefaultConfiguration(patched);

        mInjectedCount = static_cast<int>(bundled.size());
        warn(
            "Bind mount unavailable, injected {} bundled CA certificate(s) into this process only. See issue #15.",
            mInjectedCount
        );
    });
}

} // namespace mod
