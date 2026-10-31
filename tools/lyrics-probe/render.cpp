// SPDX-License-Identifier: GPL-3.0-only
// Runs the production renderer offscreen without touching the host application's window.
#include "filemanager/player/LyricLineItem.h"
#include "filemanager/player/LyricTimeline.h"
#include <QDebug>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItemGrabResult>
#include <QQuickView>
#include <QTimer>
#include <functional>
using namespace mod::filemanager;
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<LyricLineItem>("RenderCheck", 1, 0, "LyricLineItem");
    QQuickView    view;
    QQmlComponent component(view.engine());
    component.setData(
        R"(
import QtQuick 2.15
import RenderCheck 1.0
Rectangle {
    width: 320; height: 170; color: "#101216"
    Text {
        objectName: "translation"; x: 54; y: original.y + original.height + 3
        width: 256; font.family: "Noto Sans SC"; font.pixelSize: 15
        lineHeightMode: Text.FixedHeight; lineHeight: 20; wrapMode: Text.Wrap
        color: "#9096a0"; text: ""; textFormat: Text.PlainText
    }
    LyricLineItem {
        id: original
        objectName: "line"; x: 54; y: 10; width: 256; height: implicitHeight
        font.family: "Noto Sans SC"; font.pixelSize: 21; active: true
    }
})",
        QUrl()
    );
    auto* root = qobject_cast<QQuickItem*>(component.create());
    if (!root) {
        qCritical() << component.errors();
        return 1;
    }
    view.setContent(QUrl(), &component, root);
    view.resize(320, 170);
    auto*      item   = root->findChild<LyricLineItem*>("line");
    const auto parsed = parseLyrics(
        QString::fromUtf8(
            "[00:00]<00:00>Wonderful music flows over the wrapped line <00:02>中文与 é "
            "👩‍👩‍👧‍👦<00:04>"
        )
    );
    QVariantList cues;
    for (const auto& cue : parsed[0].cues)
        cues.append(
            QVariantMap{
                {"time",  cue.time },
                {"until", cue.until},
                {"end",   cue.end  }
        }
        );
    item->setLine({
        {"mainLrc", parsed[0].text},
        {"time",    0             },
        {"cues",    cues          }
    });
    view.show();
    QSharedPointer<QQuickItemGrabResult> pending;
    const QVector<int>                   times{0, 500, 1500, 2500, 4000};
    int                                  previous = -1;
    qreal                                height   = -1;
    std::function<void(int)>             capture;
    capture = [&](int index) {
        if (index == times.size()) {
            const auto* original = item;
            if (original->focusY() <= 0) {
                app.exit(5);
                return;
            }
            item->setLine({
                {"mainLrc", QString::fromUtf8("君の声が聞こえる")},
                {"time",    0                                    }
            });
            auto* translation = root->findChild<QQuickItem*>("translation");
            translation->setProperty("text", QString::fromUtf8("你的声音依然回响在我耳边，跨越漫长的夜晚。"));
            QTimer::singleShot(100, &app, [&] {
                auto*      translated = root->findChild<QQuickItem*>("translation");
                const auto gap        = translated->y() - item->y() - item->height();
                qInfo() << "BILINGUAL originalHeight=" << item->height() << "gap=" << gap
                        << "translationHeight=" << translated->height();
                if (gap < 2 || gap > 4 || item->height() > 30 || translated->height() > 42) {
                    app.exit(6);
                    return;
                }
                pending = root->grabToImage();
                QObject::connect(pending.data(), &QQuickItemGrabResult::ready, &app, [&] {
                    pending->image().save(QCoreApplication::applicationDirPath() + "/bilingual.png");
                    qInfo() << "RENDER CHECK PASS";
                    app.quit();
                });
            });
            return;
        }
        item->setPosition(times[index]);
        QTimer::singleShot(80, &app, [&, index] {
            pending = root->grabToImage();
            if (!pending) {
                app.exit(1);
                return;
            }
            QObject::connect(pending.data(), &QQuickItemGrabResult::ready, &app, [&, index] {
                const auto image = pending->image().convertToFormat(QImage::Format_ARGB32);
                int        blue  = 0;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const auto pixel = image.pixel(x, y);
                        if (qBlue(pixel) > qRed(pixel) + 50) ++blue;
                    }
                if (height < 0) height = item->implicitHeight();
                qInfo() << "RENDER time=" << times[index] << "bluePixels=" << blue
                        << "height=" << item->implicitHeight();
                if (image.isNull() || blue <= previous || item->implicitHeight() != height) {
                    app.exit(2);
                    return;
                }
                previous = blue;
                image.save(
                    QCoreApplication::applicationDirPath() + "/render-" + QString::number(times[index]) + ".png"
                );
                capture(index + 1);
            });
        });
    };
    QTimer::singleShot(100, &app, [&] { capture(0); });
    QTimer::singleShot(8000, &app, [&] { app.exit(3); });
    return app.exec();
}
