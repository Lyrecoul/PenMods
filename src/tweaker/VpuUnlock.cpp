// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/VpuUnlock.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlContext>

#include <unistd.h>

namespace mod {

namespace {

constexpr auto BOOT_DIR     = "/userdisk/PenMods";
constexpr auto STOCK_IMAGE  = "/userdisk/PenMods/boot_stock.img";
constexpr auto PATCHED_IMAGE = "/userdisk/PenMods/boot_vpu.img";

constexpr auto DT_VPU_NODE   = "/proc/device-tree/vpu_combo";
constexpr auto DT_VPU_STATUS = "/proc/device-tree/vpu_combo/status";

constexpr quint32 FDT_MAGIC       = 0xD00DFEED;
constexpr quint32 FDT_BEGIN_NODE  = 0x1;
constexpr quint32 FDT_END_NODE    = 0x2;
constexpr quint32 FDT_PROP        = 0x3;
constexpr quint32 FDT_NOP         = 0x4;
constexpr quint32 FDT_END         = 0x9;

/// 大端读写（FDT 全部是大端）
quint32 readBe32(const QByteArray& data, qsizetype offset) {
    const int at = static_cast<int>(offset);
    return (static_cast<quint32>(static_cast<quint8>(data.at(at))) << 24)
         | (static_cast<quint32>(static_cast<quint8>(data.at(at + 1))) << 16)
         | (static_cast<quint32>(static_cast<quint8>(data.at(at + 2))) << 8)
         | static_cast<quint32>(static_cast<quint8>(data.at(at + 3)));
}

void writeBe32(QByteArray& data, qsizetype offset, quint32 value) {
    const int at = static_cast<int>(offset);
    data[at]     = static_cast<char>((value >> 24) & 0xFF);
    data[at + 1] = static_cast<char>((value >> 16) & 0xFF);
    data[at + 2] = static_cast<char>((value >> 8) & 0xFF);
    data[at + 3] = static_cast<char>(value & 0xFF);
}

/// FDT 里所有值都按 4 字节对齐
qsizetype alignUp(qsizetype value) { return (value + 3) & ~static_cast<qsizetype>(3); }

qsizetype findMagic(const QByteArray& data, qsizetype from) {
    const char* found = static_cast<const char*>(memmem(data.constData() + from, data.size() - from, "\xd0\x0d\xfe\xed", 4));
    return found ? found - data.constData() : -1;
}

/**
 * 就地改写单个 FDT 里的 vpu_combo/status。
 *
 * 原值通常是 "disabled\0"（9 字节，尾部补齐到 12），也可能被写成拼错的 "okey\0"；
 * 这里只把属性值前 5 个字节写成 "okay\0"、把长度字段改成 5，其余字节清零 —— 字符串以第一个
 * NUL 结束，`of_property_read_string()` 只看长度范围内的内容，所以不需要挪动其它数据。
 */
int patchOneDtb(QByteArray& image, qsizetype base) {
    if (base + 40 > image.size()) return -1;

    const quint32 totalSize   = readBe32(image, base + 4);
    const quint32 offStruct   = readBe32(image, base + 8);
    const quint32 offStrings  = readBe32(image, base + 12);
    const quint32 sizeStrings = readBe32(image, base + 32);
    const quint32 sizeStruct  = readBe32(image, base + 36);
    if (offStruct + sizeStruct > totalSize || offStrings + sizeStrings > totalSize) return -1;

    const qsizetype structOff  = base + offStruct;
    const qsizetype structEnd  = structOff + sizeStruct;
    const qsizetype stringsOff = base + offStrings;

    int  patched     = 0;
    int  vpuDepth    = -1; // 进入 vpu_combo 节点时的深度（-1 表示不在该节点里）
    int  depth       = 0;
    qsizetype cursor = structOff;

    while (cursor + 4 <= structEnd) {
        const quint32 token = readBe32(image, cursor);
        cursor += 4;

        if (token == FDT_BEGIN_NODE) {
            qsizetype end = image.indexOf('\0', cursor);
            if (end < 0 || end >= structEnd) return -1;
            const QByteArray name = image.mid(cursor, end - cursor);
            depth += 1;
            if (vpuDepth < 0 && name == "vpu_combo") vpuDepth = depth;
            cursor = alignUp(end + 1);
        } else if (token == FDT_END_NODE) {
            if (depth == vpuDepth) vpuDepth = -1;
            depth -= 1;
        } else if (token == FDT_PROP) {
            if (cursor + 8 > structEnd) return -1;
            const quint32 length  = readBe32(image, cursor);
            const quint32 nameOff = readBe32(image, cursor + 4);
            const qsizetype valueOff = cursor + 8;
            if (valueOff + length > structEnd) return -1;

            if (vpuDepth >= 0) {
                if (stringsOff + nameOff >= base + totalSize) return -1;
                qsizetype nameEnd = image.indexOf('\0', stringsOff + nameOff);
                if (nameEnd < 0) return -1;
                const QByteArray propName = image.mid(stringsOff + nameOff, nameEnd - stringsOff - nameOff);

                if (propName == "status") {
                    QByteArray value = image.mid(valueOff, length);
                    const int  nul   = value.indexOf('\0');
                    if (nul >= 0) value.truncate(nul);
                    // 只认内核 of_device_is_available() 认可的 "okay"/"ok"；
                    // "okey"（拼写错）也要当成未启用，否则会出现"看起来打了补丁其实没有"
                    if (value != "okay" && value != "ok") {
                        // 只改值、**不动长度字段**：FDT 里属性值的 4 字节补齐长度取决于 length，
                        // 把 9 改成 5 会让后续 token 的起始位置前移 4 字节、整个 blob 就坏了。
                        // 保留 length=9、值写成 "okay\0\0\0\0"，字符串仍以第一个 NUL 结束，
                        // of_device_is_available() 的 strcmp 一样成立，而且镜像结构完全不变。
                        if (length < 5) return -1;
                        image.replace(valueOff, 5, QByteArray("okay\0", 5));
                        for (qsizetype i = 5; i < length; ++i) image[static_cast<int>(valueOff + i)] = '\0';
                        patched += 1;
                    }
                }
            }
            cursor = alignUp(valueOff + length);
        } else if (token == FDT_NOP) {
            continue;
        } else if (token == FDT_END) {
            break;
        } else {
            return -1;
        }
    }
    return patched;
}

} // namespace

VpuUnlock::VpuUnlock() : Logger("VpuUnlock") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("vpuUnlock", this);
    });
}

bool VpuUnlock::enabled() const {
    QFile status(DT_VPU_STATUS);
    if (!status.open(QIODevice::ReadOnly)) return false;
    const QByteArray value = status.readAll().left(4);
    return value == "okay" || value == "ok";
}

bool VpuUnlock::supported() {
    if (geteuid() != 0) return false;
    if (!QFile::exists(DT_VPU_NODE)) return false; // 该机型设备树没有这个节点，补丁没有意义
    const QString partition = bootPartition();
    if (partition.isEmpty()) return false;
    QFileInfo info(partition);
    if (!info.exists() || !info.isWritable()) return false;
    return QDir().mkpath(BOOT_DIR);
}

QString VpuUnlock::bootPartition() const {
    // A/B 设备按 slot_suffix 选 boot_a / boot_b，非 A/B 设备退化成 boot
    QFile cmdline("/proc/cmdline");
    if (cmdline.open(QIODevice::ReadOnly)) {
        const QString  line = QString::fromLatin1(cmdline.readAll());
        const QString  slot = line.section("androidboot.slot_suffix=", 1).section(' ', 0, 0).trimmed();
        if (!slot.isEmpty()) {
            const QString byName = "/dev/block/by-name/boot" + slot;
            if (QFile::exists(byName)) return byName;
        }
    }
    if (QFile::exists("/dev/block/by-name/boot")) return "/dev/block/by-name/boot";
    return "/dev/block/by-name/boot_a";
}

void VpuUnlock::setEnabled(bool value) {
    if (!supported()) {
        fail("VPU 解锁不可用：需要 root、可写的 boot 分区和 vpu_combo 节点");
        return;
    }
    if (value == enabled()) return;

    QString error;

    if (value) {
        // 1. 出厂镜像只备份一次，之后所有回滚都用它
        if (!backupFactoryImage(error)) {
            fail(error);
            return;
        }

        // 2. 生成补丁镜像
        if (!makePatchedImage(error)) {
            fail("生成 VPU 补丁镜像失败：" + error);
            return;
        }

        // 3. 刷入 + 回读校验；失败立刻恢复出厂镜像
        if (!flashImage(PATCHED_IMAGE, error)) {
            QString ignored;
            flashImage(STOCK_IMAGE, ignored);
            fail("刷入 VPU 补丁失败，已恢复出厂 boot：" + error);
            return;
        }
        info("VPU unlock applied; rebooting to take effect.");
    } else {
        if (!QFile::exists(STOCK_IMAGE)) {
            fail("缺少出厂 boot 备份，无法恢复");
            return;
        }
        if (!flashImage(STOCK_IMAGE, error)) {
            fail("恢复出厂 boot 失败：" + error);
            return;
        }
        info("VPU unlock removed; rebooting to take effect.");
    }

    emit stateChanged();
    rebootSystem();
}

/// 把当前 boot 分区内容存成出厂镜像（只在第一次开启时做）
bool VpuUnlock::backupFactoryImage(QString& error) {
    if (QFile::exists(STOCK_IMAGE)) return true;

    QFile source(bootPartition());
    if (!source.open(QIODevice::ReadOnly)) {
        error = QString("备份出厂 boot 失败：无法读取 %1").arg(bootPartition());
        return false;
    }
    const QByteArray stock = source.readAll();
    source.close();
    if (stock.size() < 1024 * 1024) {
        error = "备份出厂 boot 失败：读到的镜像过小";
        return false;
    }

    QFile target(STOCK_IMAGE);
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate) || target.write(stock) != stock.size()) {
        target.close();
        QFile::remove(STOCK_IMAGE);
        error = QString("备份出厂 boot 失败：无法写入 %1").arg(STOCK_IMAGE);
        return false;
    }
    target.close();
    info("Backed up factory boot image to {} ({} bytes)", STOCK_IMAGE, stock.size());
    return true;
}

bool VpuUnlock::makePatchedImage(QString& error) {
    QFile source(STOCK_IMAGE);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QString("无法读取 %1").arg(STOCK_IMAGE);
        return false;
    }
    QByteArray image = source.readAll();
    source.close();

    const int patched = patchDtbs(image);
    if (patched < 0) {
        error = "boot 镜像里的设备树布局无法识别";
        return false;
    }
    if (patched == 0) {
        error = "没有找到可改写的 vpu_combo/status（可能已经打过补丁）";
        return false;
    }

    QFile target(PATCHED_IMAGE);
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate) || target.write(image) != image.size()) {
        target.close();
        QFile::remove(PATCHED_IMAGE);
        error = QString("无法写入 %1").arg(PATCHED_IMAGE);
        return false;
    }
    target.close();

    // 复核：再读一遍补丁镜像，应当已经没有需要改的节点
    QFile check(PATCHED_IMAGE);
    if (!check.open(QIODevice::ReadOnly)) {
        error = "补丁镜像复核失败";
        return false;
    }
    QByteArray verify = check.readAll();
    check.close();
    if (patchDtbs(verify) != 0) {
        error = "补丁镜像复核失败（仍有未改写的节点）";
        return false;
    }

    info("Patched {} device tree status propert{} in {} ({} bytes)",
         patched, patched == 1 ? "y" : "ies", PATCHED_IMAGE, image.size());
    return true;
}

int VpuUnlock::patchDtbs(QByteArray& image) {
    int patched = 0;
    for (qsizetype off = 0;;) {
        off = findMagic(image, off);
        if (off < 0) break;
        const int result = patchOneDtb(image, off);
        if (result < 0) return -1;
        patched += result;
        off += 4;
    }
    return patched;
}

bool VpuUnlock::flashImage(const QString& image, QString& error) const {
    const QString partition = bootPartition();

    QFile source(image);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QString("无法读取 %1").arg(image);
        return false;
    }
    const QByteArray payload = source.readAll();
    source.close();
    if (payload.isEmpty()) {
        error = "镜像为空";
        return false;
    }

    const std::string cmd = QString("dd if=%1 of=%2 bs=4096 conv=fsync 2>/dev/null && sync").arg(image, partition).toStdString();
    exec(cmd.c_str());

    QFile verify(partition);
    if (!verify.open(QIODevice::ReadOnly)) {
        error = "回读 boot 分区失败";
        return false;
    }
    const QByteArray readBack = verify.read(payload.size());
    verify.close();

    if (readBack.size() != payload.size()) {
        error = "回读长度不一致";
        return false;
    }
    if (QCryptographicHash::hash(readBack, QCryptographicHash::Md5)
        != QCryptographicHash::hash(payload, QCryptographicHash::Md5)) {
        error = "回读校验不一致";
        return false;
    }
    return true;
}

void VpuUnlock::fail(const QString& message) {
    warn("{}", message.toStdString());
    showToast(message.toStdString(), "#E9900C");
    emit stateChanged();
}

void VpuUnlock::rebootSystem() {
    // 设备树只在启动时解析，所以只能重启整机；reboot 会杀掉本进程，
    // 因此放到后台子 shell 里延迟一秒执行，先让 UI 与日志收尾。
    exec("sync; (sleep 1; reboot) >/dev/null 2>&1 &");
}

} // namespace mod
