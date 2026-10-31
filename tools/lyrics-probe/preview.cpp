// SPDX-License-Identifier: GPL-3.0-only
#include <QDebug>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <cstdio>
class LyricsStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool lyricBusy MEMBER busy NOTIFY changed)
    Q_PROPERTY(QString lyricPreview MEMBER preview NOTIFY changed)
    Q_PROPERTY(QString lyricStatus MEMBER status NOTIFY changed)
    Q_PROPERTY(QVariantList lyricCandidates MEMBER candidates NOTIFY changed)
public:
    bool         busy    = false;
    QString      preview = QStringLiteral("Preview line\nTranslation\nLong preview\nMore lyrics\nEnd");
    QString      status;
    QVariantList candidates{
        QVariantMap{{"title", "Candidate"}, {"detail", "Artist"}}
    };
    int                 downloads = 0, saves = 0, cancels = 0;
    Q_INVOKABLE QString lyricSearchKeyword() { return "Song"; }
    Q_INVOKABLE void    searchOnlineLyrics(const QString&) {}
    Q_INVOKABLE void    saveOnlineLyrics(int) {
        ++downloads;
        preview.clear();
        emit changed();
    }
    Q_INVOKABLE void commitOnlineLyrics() { ++saves; }
    Q_INVOKABLE void cancelOnlineLyrics() { ++cancels; }
signals:
    void changed();
    void onlineLyricsChanged();
};
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    qmlRegisterModule("com.youdao.pen", 1, 0);
    LyricsStub model;
    QQuickView view;
    view.rootContext()->setContextProperty("musicPlayer", &model);
    view.rootContext()->setContextProperty(
        "qmlGlobal",
        QVariantMap{
            {"fontFamily",       "Noto Sans SC"},
            {"inputPageShowing", false         }
    }
    );
    view.setSource(QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + "/audioplayer/YOnlineLyricsPanel.qml"));
    auto* panel = view.rootObject();
    if (!panel) return 1;
    view.resize(320, 170);
    panel->setVisible(true);
    panel->setProperty("previewing", true);
    view.show();
    auto tap = [&](int x, int y) {
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(
                type,
                QPointF(x, y),
                Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                Qt::NoModifier
            );
            QCoreApplication::sendEvent(&view, &event);
        }
    };
    QTimer::singleShot(150, &app, [&] {
        // These points lie in the preview, above the candidate card and its lower edge.
        tap(270, 90);
        tap(270, 120);
        tap(270, 150);
        if (model.downloads || model.cancels || !panel->property("previewing").toBool()) {
            std::printf("PREVIEW FAIL: click through downloads=%d cancels=%d\n", model.downloads, model.cancels);
            app.exit(2);
            return;
        }
        tap(270, 42); // Lower edge of the enlarged save button.
        if (model.saves != 1) {
            std::puts("PREVIEW FAIL: save lower edge missed");
            app.exit(3);
            return;
        }
        tap(270, 51); // Gap under toolbar must be inert.
        if (model.saves != 1 || model.downloads || model.cancels) {
            std::puts("PREVIEW FAIL: toolbar gap activates lower layer");
            app.exit(4);
            return;
        }
        tap(25, 30); // Preview back returns to candidates, without closing online lyrics.
        if (!panel->isVisible() || panel->property("previewing").toBool() || model.cancels) {
            std::puts("PREVIEW FAIL: back exits online lyrics instead of preview");
            app.exit(5);
            return;
        }
        std::puts("PREVIEW CHECK PASS: background blocked, save edge works, back stays in search");
        app.quit();
    });
    QTimer::singleShot(3000, &app, [&] { app.exit(6); });
    return app.exec();
}
#include "moc_preview.cpp"
