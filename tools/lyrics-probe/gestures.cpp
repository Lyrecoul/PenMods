// SPDX-License-Identifier: GPL-3.0-only
// Real QQuickWindow mouse events, confined to an offscreen window; never touches the pen UI.
#include "filemanager/player/LyricLineItem.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItemGrabResult>
#include <QQuickView>
#include <QTimer>
#include <functional>
using mod::filemanager::LyricLineItem;
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<LyricLineItem>("GestureCheck", 1, 0, "LyricLineItem");
    QQuickView    window;
    QQmlComponent component(window.engine());
    component.setData(
        R"(
import QtQuick 2.15
import GestureCheck 1.0
import "."
Rectangle {
    id: rootFixture
    width: 320; height: 170; color: "#101216"
    property int taps: 0
    property bool longProbe: false
    property real longFocus: 0
    property bool fadeProbe: false
    YAudioPlayerLyricView {
        id: viewport; objectName: "viewport"; x: 54; width: 256; height: 158
        title: "Pinned Test.wav"; fontFamily: "Noto Sans SC"; activeIndex: 0
        backgroundColor: "#101216"
        model: rootFixture.fadeProbe ? 1 : 25
        onTapped: ++parent.taps
        delegate: Loader {
            width: viewport.width
            height: item ? item.implicitHeight : 0
            property real readingOffset: rootFixture.longProbe ? rootFixture.longFocus : 0
            onReadingOffsetChanged: if (index === viewport.activeIndex) viewport.followCurrent()
            sourceComponent: rootFixture.fadeProbe || rootFixture.longProbe ? solid : lyric
            Component { id: solid; Rectangle { color: "white"; implicitHeight: 400; width: viewport.width } }
            Component { id: lyric; LyricLineItem {
                width: viewport.width; height: implicitHeight
                line: ({mainLrc: "Line " + index + " 中文歌词换行测试", time: 0, cues: []})
                font.family: "Noto Sans SC"; font.pixelSize: 21
                active: index === viewport.activeIndex
            } }
        }
    }
})",
        QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + "/Fixture.qml")
    );
    auto* root = qobject_cast<QQuickItem*>(component.create());
    if (!root) {
        qCritical() << component.errors();
        return 1;
    }
    window.setContent(QUrl(), &component, root);
    window.resize(320, 170);
    window.show();
    auto* viewport = root->findChild<QQuickItem*>("viewport");
    auto* list     = root->findChild<QQuickItem*>("lyricViewport");
    if (!viewport || !list) return 2;
    auto check = [&](bool ok, const char* why) {
        if (!ok) {
            qCritical() << "GESTURE FAIL" << why;
            std::exit(3);
        }
    };
    QElapsedTimer clock;
    clock.start();
    auto mouse = [&](QEvent::Type type, int x, int y) {
        const auto  button  = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const auto  buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, QPointF(x, y), QPointF(x, y), QPointF(x, y), button, buttons, Qt::NoModifier);
        event.setTimestamp(clock.elapsed());
        QCoreApplication::sendEvent(&window, &event);
    };
    QImage                               titleBefore;
    QSharedPointer<QQuickItemGrabResult> grab;
    auto snapshot = [&](const QString& name, std::function<void(const QImage&)> callback) {
        grab = root->grabToImage();
        check(!grab.isNull(), "snapshot unavailable");
        QObject::connect(grab.data(), &QQuickItemGrabResult::ready, &app, [&, name, callback] {
            const auto image = grab->image();
            image.save(QCoreApplication::applicationDirPath() + "/" + name + ".png");
            callback(image);
        });
    };
    qreal                          beforeDrag = 0, heldY = 0;
    bool                           coast = false;
    QVector<std::function<void()>> steps = {
        [&] {
            check(
                list->y() >= 30 && list->y() <= 32 && list->height() >= 126 && list->clip(),
                "lyrics must stay clipped below the fixed title"
            );
            snapshot("before", [&](const QImage& image) { titleBefore = image.copy(54, 0, 256, 30); });
        },
        [&] { mouse(QEvent::MouseButtonPress, 180, 80); },
        [&] { mouse(QEvent::MouseButtonRelease, 180, 80); },
        [&] {
            check(root->property("taps").toInt() == 1, "ordinary tap must pause once");
            beforeDrag = list->property("contentY").toReal();
            mouse(QEvent::MouseButtonPress, 180, 125);
        },
        [&] { mouse(QEvent::MouseMove, 180, 105); },
        [&] { mouse(QEvent::MouseMove, 180, 75); },
        [&] { mouse(QEvent::MouseMove, 180, 44); },
        [&] { mouse(QEvent::MouseButtonRelease, 180, 44); },
        [&] {
            check(root->property("taps").toInt() == 1, "vertical drag must not pause");
            check(list->property("contentY").toReal() > beforeDrag + 10, "vertical drag must scroll");
            check(viewport->property("browsing").toBool(), "drag must suspend follow");
            coast = list->property("flicking").toBool();
            mouse(QEvent::MouseButtonPress, 180, 80);
        },
        [&] { mouse(QEvent::MouseButtonRelease, 180, 80); },
        [&] {
            if (coast) check(root->property("taps").toInt() == 1, "tap during coast must only stop motion");
            QMetaObject::invokeMethod(list, "cancelFlick");
            viewport->setProperty("activeIndex", 8);
            heldY = list->property("contentY").toReal();
        },
        [&] {
            check(list->property("currentIndex").toInt() == -1, "song progress must not grab browsing back");
            check(qAbs(list->property("contentY").toReal() - heldY) < 1, "browse position must hold");
            mouse(QEvent::MouseButtonPress, 90, 80);
        },
        [&] { mouse(QEvent::MouseMove, 145, 80); },
        [&] { mouse(QEvent::MouseMove, 215, 80); },
        [&] { mouse(QEvent::MouseButtonRelease, 215, 80); },
        [&] {
            check(root->property("taps").toInt() == (coast ? 1 : 2), "horizontal swipe must not pause");
            qInfo() << "GESTURE drag/tap/browse PASS coast=" << coast;
            QTimer::singleShot(3400, &app, [&] {
                check(!viewport->property("browsing").toBool(), "idle browsing must resume follow");
                check(list->property("currentIndex").toInt() == 8, "follow must use latest active line");
                const auto* current = qvariant_cast<QQuickItem*>(list->property("currentItem"));
                const qreal top     = list->property("contentY").toReal();
                check(
                    current && current->y() >= top - 1 && current->y() < top + list->height(),
                    "resumed current line must actually be visible below the title"
                );
                snapshot("after", [&](const QImage& image) {
                    check(
                        !titleBefore.isNull() && image.copy(54, 0, 256, 30) == titleBefore,
                        "scrolling native lyric textures must not alter title pixels"
                    );
                    viewport->setProperty("lyricsVisible", false);
                    QTimer::singleShot(80, &app, [&] {
                        snapshot("empty", [&](const QImage& empty) {
                            check(empty.copy(54, 0, 256, 30) == titleBefore, "empty lyrics must preserve the title");
                            auto* title = qvariant_cast<QQuickItem*>(viewport->property("titleItem"));
                            check(title && title->isVisible(), "empty lyrics must not hide the title item");
                            viewport->setProperty("lyricsVisible", true);
                            viewport->setProperty("browsing", true);
                            root->setProperty("fadeProbe", true);
                            QTimer::singleShot(150, &app, [&] {
                                list->setProperty("contentY", 50);
                                QTimer::singleShot(80, &app, [&] {
                                    snapshot("fade", [&](const QImage& fade) {
                                        const int top    = qRound(list->y()),
                                                  bottom = qRound(list->y() + list->height()) - 1;
                                        const int middle = (top + bottom) / 2;
                                        check(
                                            qRed(fade.pixel(180, top)) < 60 && qRed(fade.pixel(180, bottom)) < 60,
                                            "both viewport edges must fade to the background"
                                        );
                                        check(
                                            qRed(fade.pixel(180, middle)) > 240,
                                            "edge fades must preserve centre content"
                                        );
                                        check(
                                            qRed(fade.pixel(180, top + 4)) > 60 && qRed(fade.pixel(180, top + 4)) < 240,
                                            "edge transition must contain intermediate opacity"
                                        );
                                        root->setProperty("longProbe", true);
                                        root->setProperty("fadeProbe", false);
                                        root->setProperty("longFocus", 260);
                                        viewport->setProperty("browsing", false);
                                        QMetaObject::invokeMethod(viewport, "followCurrent");
                                        QTimer::singleShot(400, &app, [&] {
                                            const auto* longItem =
                                                qvariant_cast<QQuickItem*>(list->property("currentItem"));
                                            const qreal offset = list->property("contentY").toReal();
                                            check(
                                                longItem && longItem->y() + 260 > offset
                                                    && longItem->y() + 260 < offset + list->height(),
                                                "current singing row of a long line must stay visible"
                                            );
                                            qInfo() << "GESTURE CHECK PASS";
                                            app.quit();
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        }
    };
    int    stage = 0;
    QTimer sequence;
    sequence.setInterval(40);
    QObject::connect(&sequence, &QTimer::timeout, &app, [&] {
        steps[stage++]();
        if (stage == steps.size()) sequence.stop();
    });
    QTimer::singleShot(200, &app, [&] { sequence.start(); });
    QTimer::singleShot(10000, &app, [&] { app.exit(4); });
    return app.exec();
}
