// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "system/input/ScreenGrabber.h"

#include "common/Event.h"

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QQmlContext>
#include <QScopeGuard>
#include <QThread>

namespace mod {

namespace {

constexpr const char* kReq = "/tmp/penmods_shot";
constexpr const char* kLog = "/tmp/penmods_shot.log";

} // namespace

ScreenGrabber::ScreenGrabber() : Logger("ScreenGrabber") {

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext*) {
        mView = &view;
        info("宿主窗口已就绪 {}x{}", view.width(), view.height());
    });

    connect(&Event::getInstance(), &Event::uiCompleted, this, [this]() {
        if (!mTimer) return;
        mTimer->start();
        info("轮询已启动 {}ms", mTimer->interval());
    });

    // 1 秒一次，代价只是两次 stat()。不用 QFileSystemWatcher 是因为设备上
    // inotify 对 /tmp（tmpfs）不一定可靠，而这个轮询足够省。
    mTimer = new QTimer(this);
    mTimer->setInterval(1000);
    connect(mTimer, &QTimer::timeout, this, &ScreenGrabber::tick);
}

void ScreenGrabber::tick() {
    // 心跳放在最前面：**无论后面哪个条件不满足，这条都能证明 Timer 真的在跑**。
    // 排查时先看心跳有没有，能立刻把「Timer 没跑」和「跑了但条件不满足」分开。
    if (++mTick % 30 == 0) debug("心跳 tick={} view={}", mTick, mView ? "yes" : "no");

    if (mBusy || !mView) return;
    if (!QFile::exists(kReq)) return;

    // 先删哨兵再抓：grabWindow + save 是同步的，删早了怕丢请求，删晚了同一帧会触发两次
    // —— 先删，失败最多丢一次请求，可接受。
    QFile::remove(QString::fromUtf8(kReq));
    info("命中哨兵，抓一帧");
    grabNow();
}

QString ScreenGrabber::grabNow() {

    mBusy = true;
    // maybe_unused：guard 的作用在**析构时**执行（每个 return 分支都会释放 mBusy），
    // 变量本身读不到，编译器会报 unused-variable。
    [[maybe_unused]] auto guard = qScopeGuard([this] { mBusy = false; });

    auto logLine = [](const QString& s) {
        QFile f(QString::fromUtf8(kLog));
        if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) f.write((s + '\n').toUtf8());
    };

    if (!mView) {
        logLine(QStringLiteral("err no-view"));
        return QStringLiteral("no-view");
    }

    // grabWindow 必须在 GUI 线程。正常情况下 QTimer 就在 GUI 线程，这里显式校验一次
    // —— 万一将来从别的线程调进来，静默失败会很难查。
    if (QThread::currentThread() != mView->thread()) {
        logLine(QStringLiteral("err wrong-thread %1 vs %2")
                    .arg(quintptr(QThread::currentThread()), 0, 16)
                    .arg(quintptr(mView->thread()), 0, 16));
        return QStringLiteral("wrong-thread");
    }

    QImage img = mView->grabWindow();
    if (img.isNull()) {
        logLine(QStringLiteral("err grab-null"));
        return QStringLiteral("grab-null");
    }

    // 设备上没有 PNG 编码器的话会写出 0 字节文件，症状和「抓图失败」一模一样，极易误判，
    // 所以先确认再保存。
    if (!QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("png"))) {
        logLine(QStringLiteral("err no-png-enc fmt=%1")
                    .arg(QString::fromLatin1(QImageWriter::supportedImageFormats().join(','))));
        return QStringLiteral("no-png-enc");
    }

    const QString path = QStringLiteral("/tmp/penmods_shot-%1.png").arg(mSeq++);
    if (!img.save(path, "PNG")) {
        logLine(QStringLiteral("err save-fail %1").arg(path));
        return QStringLiteral("save-fail");
    }

    // exposed 只记录下来、不当门槛：本机实测抓图成功时 isExposed() 仍为 false
    // （Qt 在这个平台收不到 exposure 事件），拿它判断「能不能截」会永远截不到。
    logLine(QStringLiteral("ok %1 %2x%3 exposed=%4")
                .arg(path)
                .arg(img.width())
                .arg(img.height())
                .arg(mView->isExposed() ? 1 : 0));
    info("已抓取 {}", path.toStdString());
    return path;
}

} // namespace mod
