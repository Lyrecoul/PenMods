// SPDX-License-Identifier: GPL-3.0-only
// Throwaway rendering comparison: shared QTextLayout, QML duplicate text vs cached scene graph textures.
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickView>
#include <QSGSimpleTextureNode>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <sys/resource.h>

struct Fragment {
    int    start, end;
    QRectF rect;
};
class Layout : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList lines READ lines NOTIFY changed)
    Q_PROPERTY(QVariantList bands READ bands NOTIFY bandsChanged)
    Q_PROPERTY(int sample READ sample WRITE setSample NOTIFY changed)
    Q_PROPERTY(double time READ time WRITE setTime NOTIFY bandsChanged)
    Q_PROPERTY(int height READ height NOTIFY changed)
    Q_PROPERTY(QString family READ family CONSTANT)
public:
    explicit Layout(QObject* parent = nullptr) : QObject(parent) { rebuild(); }
    QString      family() const { return QStringLiteral("Noto Sans SC"); }
    int          sample() const { return mSample; }
    double       time() const { return mTime; }
    int          height() const { return mHeight; }
    QVariantList lines() const { return mLines; }
    QVariantList bands() const { return mBands; }
    QImage       base, bright;
    int          generation = 0;
    void         setSample(int value) {
        mSample = (value % 5 + 5) % 5;
        rebuild();
        emit changed();
        setTime(mTime);
    }
    void setTime(double value) {
        mTime = value;
        mBands.clear();
        const double    progress = qBound(0.0, value / 6000.0, 1.0) * mClusters;
        QVector<QRectF> rects;
        for (const auto& fragment : fragments) {
            const double fraction = qBound(0.0, (progress - fragment.start) / (fragment.end - fragment.start), 1.0);
            if (fraction <= 0) continue;
            QRectF rect = fragment.rect;
            // Negative width records RTL advance; reveal from the visual right.
            if (rect.width() < 0) {
                rect = QRectF(rect.x() + rect.width() * fraction, rect.y(), -rect.width() * fraction, rect.height());
            } else rect.setWidth(rect.width() * fraction);
            if (rect.width() > 0.001) rects.append(rect);
        }
        std::sort(rects.begin(), rects.end(), [](const QRectF& a, const QRectF& b) {
            return a.y() == b.y() ? a.x() < b.x() : a.y() < b.y();
        });
        QVector<QRectF> merged;
        for (const auto& rect : rects) {
            if (!merged.isEmpty() && merged.last().y() == rect.y() && rect.left() <= merged.last().right() + 0.05)
                merged.last() = merged.last().united(rect);
            else merged.append(rect);
        }
        for (const auto& rect : merged)
            mBands.append(
                QVariantMap{
                    {"x", rect.x()     },
                    {"y", rect.y()     },
                    {"w", rect.width() },
                    {"h", rect.height()}
            }
            );
        emit bandsChanged();
    }
signals:
    void changed();
    void bandsChanged();

private:
    void rebuild() {
        static const QStringList samples = {
            QStringLiteral("让每一个字，随着音乐慢慢亮起。"),
            QStringLiteral("Hello wonderful world, keep singing across the next visual line."),
            QString::fromUtf8("中文与 English 混排，é 和 👩‍👩‍👧‍👦 不应拆开。"),
            QStringLiteral("超长歌词需要自然换行，并让高亮准确地跨过每一行，而不是扫过整块矩形。"),
            QString::fromUtf8("office café العربية 中文：ligature & bidi probe")
        };
        const QString text = samples[mSample];
        QFont         font(family());
        font.setPixelSize(21);
        QTextLayout layout(text, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        mLines.clear();
        fragments.clear();
        QVector<int>        boundaries{0};
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
        while (finder.toNextBoundary() >= 0) boundaries.append(finder.position());
        mClusters = boundaries.size() - 1;
        layout.beginLayout();
        int y = 0;
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(256);
            line.setPosition(QPointF(0, y));
            const int end = line.textStart() + line.textLength();
            mLines.append(
                QVariantMap{
                    {"text", text.mid(line.textStart(), line.textLength())},
                    {"y", y},
                    {"h", 29}
            }
            );
            for (int i = 0; i < mClusters; ++i) {
                if (boundaries[i] < line.textStart() || boundaries[i] >= end) continue;
                const qreal x1 = line.cursorToX(boundaries[i]);
                const qreal x2 = line.cursorToX(qMin(boundaries[i + 1], end));
                fragments.append({i, i + 1, QRectF(x1, y, x2 - x1, 29)});
            }
            y += 29;
        }
        layout.endLayout();
        mHeight = y;
        base    = QImage(256, y, QImage::Format_ARGB32_Premultiplied);
        base.fill(Qt::transparent);
        bright = base.copy();
        {
            QPainter painter(&base);
            painter.setPen(QColor("#606671"));
            layout.draw(&painter, QPointF());
        }
        {
            QPainter painter(&bright);
            painter.setPen(QColor("#8ac8ff"));
            layout.draw(&painter, QPointF());
        }
        ++generation;
        qInfo().noquote() << "LAYOUT sample=" << mSample << "rows=" << mLines.size() << "clusters=" << mClusters
                          << "texture_bytes=" << base.sizeInBytes() + bright.sizeInBytes();
    }
    int               mSample = 0, mHeight = 0, mClusters = 0;
    double            mTime = 0;
    QVariantList      mLines, mBands;
    QVector<Fragment> fragments;
};

struct CachedNode : QSGNode {
    QSGTexture *                   base = nullptr, *bright = nullptr;
    int                            generation = -1;
    QVector<QSGSimpleTextureNode*> bands;
    ~CachedNode() override {
        delete base;
        delete bright;
    }
};
class CachedLine : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject* layout READ layout WRITE setLayout NOTIFY layoutChanged)
public:
    CachedLine() { setFlag(ItemHasContents, true); }
    QObject* layout() const { return mLayout; }
    void     setLayout(QObject* object) {
        if (mLayout) disconnect(mLayout, nullptr, this, nullptr);
        mLayout = qobject_cast<Layout*>(object);
        if (mLayout) {
            connect(mLayout, &Layout::changed, this, [this] { update(); });
            connect(mLayout, &Layout::bandsChanged, this, [this] { update(); });
        }
        emit layoutChanged();
        update();
    }
signals:
    void layoutChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override {
        auto* node = static_cast<CachedNode*>(old);
        if (!mLayout) {
            delete node;
            return nullptr;
        }
        if (!node || node->generation != mLayout->generation) {
            delete node;
            node             = new CachedNode;
            node->generation = mLayout->generation;
            node->base       = window()->createTextureFromImage(mLayout->base);
            node->bright     = window()->createTextureFromImage(mLayout->bright);
            auto* base       = new QSGSimpleTextureNode;
            base->setTexture(node->base);
            base->setRect(0, 0, 256, mLayout->height());
            node->appendChildNode(base);
        }
        const auto bands = mLayout->bands();
        while (node->bands.size() > bands.size()) {
            auto* band = node->bands.takeLast();
            node->removeChildNode(band);
            delete band;
        }
        while (node->bands.size() < bands.size()) {
            auto* band = new QSGSimpleTextureNode;
            band->setTexture(node->bright);
            node->bands.append(band);
            node->appendChildNode(band);
        }
        for (int i = 0; i < bands.size(); ++i) {
            const auto value = bands[i].toMap();
            QRectF     rect(value["x"].toDouble(), value["y"].toDouble(), value["w"].toDouble(), value["h"].toDouble());
            node->bands[i]->setRect(rect);
            node->bands[i]->setSourceRect(rect);
        }
        return node;
    }

private:
    Layout* mLayout = nullptr;
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<CachedLine>("LyricsPrototype", 1, 0, "CachedLine");
    Layout            layout;
    QQuickView        view;
    const QStringList args      = app.arguments();
    const bool        benchmark = args.contains("--benchmark");
    const bool        native    = args.contains("--native");
    view.rootContext()->setContextProperty("probe", &layout);
    view.rootContext()->setContextProperty("initialNative", native);
    view.rootContext()->setContextProperty("benchmark", benchmark);
    view.setSource(QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + "/Prototype.qml"));
    if (view.status() == QQuickView::Error) return 1;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(320, 170);
    QElapsedTimer clock;
    clock.start();
    QVector<double> intervals;
    qint64          last = 0;
    QObject::connect(
        &view,
        &QQuickWindow::frameSwapped,
        &app,
        [&] {
            const qint64 now = clock.nsecsElapsed();
            if (last && now > 1000000000LL) intervals.append((now - last) / 1000000.0);
            last = now;
        },
        Qt::QueuedConnection
    );
    view.show();
    rusage cpuStart{};
    QTimer::singleShot(1000, &app, [&] { getrusage(RUSAGE_SELF, &cpuStart); });
    std::function<void(int)> capture;
    capture = [&](int sample) {
        if (sample == 5) {
            app.quit();
            return;
        }
        layout.setSample(sample);
        layout.setTime(3000);
        QTimer::singleShot(100, &app, [&, sample] {
            auto grab = view.rootObject()->grabToImage();
            if (!grab) {
                qWarning() << "GRAB unavailable";
                app.exit(2);
                return;
            }
            QObject::connect(grab.data(), &QQuickItemGrabResult::ready, &app, [&, grab, sample] {
                const auto    image = grab->image();
                const QString stem  = QCoreApplication::applicationDirPath() + (native ? "/native" : "/qml");
                const bool    saved = image.save(stem + "-" + QString::number(sample) + ".png");
                if (sample == 2) image.save(stem + ".png");
                qInfo() << "GRAB sample=" << sample << image.size() << "saved=" << saved;
                if (!saved) {
                    app.exit(2);
                    return;
                }
                capture(sample + 1);
            });
        });
    };
    if (benchmark) {
        QTimer::singleShot(11000, &app, [&] {
            std::sort(intervals.begin(), intervals.end());
            const double p50 = intervals.isEmpty() ? 0 : intervals[intervals.size() / 2];
            const double p95 =
                intervals.isEmpty() ? 0 : intervals[qMin(intervals.size() - 1, int(intervals.size() * .95))];
            rusage cpuEnd{};
            getrusage(RUSAGE_SELF, &cpuEnd);
            const auto cpuMs = [](const rusage& usage) {
                return usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0 + usage.ru_stime.tv_sec * 1000.0
                     + usage.ru_stime.tv_usec / 1000.0;
            };
            qInfo().noquote() << "RESULT" << (native ? "native" : "qml") << "frames=" << intervals.size()
                              << "interval_p50_ms=" << p50 << "interval_p95_ms=" << p95
                              << "cpu_ms_after_warmup=" << cpuMs(cpuEnd) - cpuMs(cpuStart);
            QFile status("/proc/self/status");
            if (status.open(QIODevice::ReadOnly)) {
                for (const auto& line : status.readAll().split('\n'))
                    if (line.startsWith("VmRSS:") || line.startsWith("VmHWM:")) qInfo().noquote() << line;
            }
            view.rootObject()->setProperty("capturing", true);
            view.rootObject()->setProperty("paused", true);
            capture(0);
        });
    }
    return app.exec();
}
#include "prototype.moc"
