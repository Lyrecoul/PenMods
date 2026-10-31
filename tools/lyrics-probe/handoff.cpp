// SPDX-License-Identifier: GPL-3.0-only
#include "filemanager/player/LyricLineItem.h"
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickItemGrabResult>
#include <QQuickView>
#include <QTimer>
#include <cstdio>
#include <functional>
using namespace mod::filemanager;
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<LyricLineItem>("RenderCheck", 1, 0, "LyricLineItem");
    QQuickView view;
    view.rootContext()->setContextProperty(
        "YEnum",
        QVariantMap{
            {"LS_BILINGUAL", 0},
            {"LS_ORIGINAL",  1}
    }
    );
    view.rootContext()->setContextProperty(
        "YColors",
        QVariantMap{
            {"white",    "#ffffff"},
            {"blueText", "#8ac8ff"}
    }
    );
    view.setSource(QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + "/Handoff.qml"));
    if (!view.rootObject()) return 1;
    view.resize(320, 170);
    view.show();
    auto*                                     root = view.rootObject();
    QSharedPointer<QQuickItemGrabResult>      pending;
    std::function<void(const QString&, bool)> capture;
    capture = [&](const QString& name, bool done) {
        pending = root->grabToImage();
        QObject::connect(pending.data(), &QQuickItemGrabResult::ready, &app, [&, name, done] {
            auto image = pending->image();
            int  blue = 0, white = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    auto rgb = image.pixel(x, y);
                    if (qBlue(rgb) > 30 && qBlue(rgb) > qRed(rgb) * 1.25) ++blue;
                    else if (qRed(rgb) > 30 && qAbs(qRed(rgb) - qBlue(rgb)) < 10) ++white;
                }
            image.save(QCoreApplication::applicationDirPath() + "/handoff-" + name + ".png");
            std::printf("HANDOFF %s blue=%d white=%d\n", qPrintable(name), blue, white);
            const bool rewound = name == "rewound";
            if ((!rewound && (blue < 100 || white > 20)) || (rewound && (blue > 20 || white < 100))) {
                std::puts("HANDOFF FAIL: completed lyric turns white during sentence transition");
                app.exit(2);
                return;
            }
            if (done) {
                std::puts("HANDOFF CHECK PASS");
                app.quit();
            } else if (name == "complete") {
                root->setProperty("activeLine", 1);
                QTimer::singleShot(40, &app, [&] { capture("transition", false); });
            } else if (name == "transition") QTimer::singleShot(220, &app, [&] { capture("settled", false); });
            else {
                root->setProperty("activeLine", -1);
                QTimer::singleShot(80, &app, [&] { capture("rewound", true); });
            }
        });
    };
    QTimer::singleShot(100, &app, [&] { capture("complete", false); });

    QTimer::singleShot(3000, &app, [&] { app.exit(3); });
    return app.exec();
}
