#pragma once
#include "SubtitleRenderer.hpp"
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <cmath>

namespace ffmpeg_player::subtitles {

class Overlay final : public QQuickItem {
public:
    explicit Overlay(QQuickItem* parent) : QQuickItem(parent) {
        setFlag(ItemHasContents, true);
        setAcceptedMouseButtons(Qt::NoButton);
        setTransformOrigin(Center);
        setZ(1);
        setVisible(false);
        windowChangedConnection_ = connect(this, &QQuickItem::windowChanged, this,
            [this](QQuickWindow* target) { watchWindow(target); });
        watchWindow(window());
    }
    ~Overlay() override {
        // QQuickItem 析构时会脱离窗口，不能再调用派生类的映射监听器。
        disconnect(windowChangedConnection_);
        disconnect(animationConnection_);
    }
    void setContent(DocumentPtr document, QString font, double scale) {
        if (document == document_ && font == font_ && scale == scale_) return;
        document_ = std::move(document);
        font_ = std::move(font);
        scale_ = scale;
        ++revision_;
        update();
    }
    void setTime(qint64 time, QSize storage) {
        if (time == time_ && storage == storage_) return;
        time_ = time;
        storage_ = storage;
        update();
    }
    std::function<void(const QString&)> reportError;
protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void geometryChange(const QRectF& current, const QRectF& previous) override {
        QQuickItem::geometryChange(current, previous);
        update();
    }
#else
    void geometryChanged(const QRectF& current, const QRectF& previous) override {
        QQuickItem::geometryChanged(current, previous);
        update();
    }
#endif
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override {
        auto* node = static_cast<Node*>(old);
        if (!document_ || !window() || !(width() > 0 && height() > 0) ||
            !std::isfinite(width()) || !std::isfinite(height())) { delete node; return nullptr; }
        const auto mapping = sceneMapping();
        const qreal densityX = std::hypot(mapping.axisX.x(), mapping.axisX.y()) * mapping.dpr;
        const qreal densityY = std::hypot(mapping.axisY.x(), mapping.axisY.y()) * mapping.dpr;
        // 旋转 90 度产生的浮点误差不应多分配一列/行像素。
        const qreal pixelsX = std::max(qreal(1), std::ceil(width() * densityX - 1e-6));
        const qreal pixelsY = std::max(qreal(1), std::ceil(height() * densityY - 1e-6));
        if (!(densityX > 0 && densityY > 0) || !std::isfinite(pixelsX) || !std::isfinite(pixelsY) ||
            !std::isfinite(mapping.origin.x()) || !std::isfinite(mapping.origin.y())) {
            delete node;
            return nullptr;
        }
        // 先限制尺寸再转为整数，避免极大缩放导致溢出；保留原有的纹理上限。
        const bool capped = pixelsX > 2048 || pixelsY > 2048;
        const qreal limit = capped ? 2048 / std::max(pixelsX, pixelsY) : 1;
        const QSize frame(std::max(1, int(std::round(pixelsX * limit))),
                          std::max(1, int(std::round(pixelsY * limit))));
        const qreal pixelWidth = capped ? width() / frame.width() : 1 / densityX;
        const qreal pixelHeight = capped ? height() / frame.height() : 1 / densityY;
        QPointF offset;
        if (mapping.axisAligned()) {
            const QPointF snapped(std::round(mapping.origin.x() * mapping.dpr) / mapping.dpr,
                                  std::round(mapping.origin.y() * mapping.dpr) / mapping.dpr);
            offset = mapFromScene(snapped);
        }
        if (!node) node = new Node;
        const auto bitmap = node->renderer.render(document_, font_, scale_, frame,
                                                  storage_.isEmpty() ? frame : storage_, time_);
        if (!bitmap.error.isEmpty()) {
            if (node->lastError != bitmap.error || node->errorRevision != revision_) {
                node->lastError = bitmap.error;
                node->errorRevision = revision_;
                const auto revision = revision_;
                QMetaObject::invokeMethod(this, [this, revision, error = bitmap.error] {
                    if (revision == revision_ && reportError) reportError(error);
                }, Qt::QueuedConnection);
            }
            node->clearBitmap();
        } else if (bitmap.changed) {
            node->lastError.clear();
            if (bitmap.image.isNull()) {
                node->clearBitmap();
            } else {
                auto* texture = window()->createTextureFromImage(bitmap.image);
                if (texture) {
                    if (!node->quad) {
                        node->quad = new QSGSimpleTextureNode;
                        node->quad->setOwnsTexture(false);
                        node->quad->setFiltering(QSGTexture::Linear);
                    }
                    auto* previous = node->ownedTexture;
                    node->quad->setTexture(texture);
                    node->ownedTexture = texture;
                    delete previous;
                    node->bitmapBounds = bitmap.bounds;
                }
            }
        }
        // 平移可能不改变 libass 位图，仍需刷新像素对齐后的几何位置。
        if (node->quad) {
            const auto& bounds = node->bitmapBounds;
            node->quad->setRect(QRectF(offset.x() + bounds.x() * pixelWidth,
                                      offset.y() + bounds.y() * pixelHeight,
                                      bounds.width() * pixelWidth, bounds.height() * pixelHeight));
            // 软件场景图会在插入节点时立即读取材质，先初始化纹理与几何再挂到树上。
            if (!node->quad->parent()) node->appendChildNode(node->quad);
        }
        return node;
    }
private:
    struct SceneMapping {
        QPointF origin, axisX, axisY;
        qreal dpr = 0;
        bool operator==(const SceneMapping& other) const {
            return origin == other.origin && axisX == other.axisX && axisY == other.axisY && dpr == other.dpr;
        }
        bool axisAligned() const {
            const qreal toleranceX = std::hypot(axisX.x(), axisX.y()) * 1e-6;
            const qreal toleranceY = std::hypot(axisY.x(), axisY.y()) * 1e-6;
            return (std::abs(axisX.y()) <= toleranceX && std::abs(axisY.x()) <= toleranceY) ||
                   (std::abs(axisX.x()) <= toleranceX && std::abs(axisY.y()) <= toleranceY);
        }
    };
    SceneMapping sceneMapping() const {
        const auto origin = mapToScene(QPointF(0, 0));
        return {origin, mapToScene(QPointF(1, 0)) - origin, mapToScene(QPointF(0, 1)) - origin,
                window() ? window()->effectiveDevicePixelRatio() : 0};
    }
    void watchWindow(QQuickWindow* target) {
        disconnect(animationConnection_);
        observedMapping_ = {};
        if (target) {
            // 动画更新后、场景图同步前检查父级变换，暂停时缩放/拖动也能刷新字幕。
            animationConnection_ = connect(target, &QQuickWindow::afterAnimating, this, [this] {
                if (!document_ || !isVisible()) return;
                const auto mapping = sceneMapping();
                if (mapping == observedMapping_) return;
                observedMapping_ = mapping;
                update();
            });
        }
        update();
    }
    class Node : public QSGNode {
    public:
        ~Node() override { clearBitmap(); }
        void clearBitmap() {
            if (quad) { removeChildNode(quad); delete quad; quad = nullptr; }
            delete ownedTexture;
            ownedTexture = nullptr;
            bitmapBounds = {};
        }
        Renderer renderer;
        QSGSimpleTextureNode* quad = nullptr;
        QSGTexture* ownedTexture = nullptr;
        QRect bitmapBounds;
        QString lastError;
        quint64 errorRevision = 0;
    };
    DocumentPtr document_;
    QString font_;
    double scale_ = 1;
    qint64 time_ = -1;
    QSize storage_;
    quint64 revision_ = 0;
    SceneMapping observedMapping_;
    QMetaObject::Connection windowChangedConnection_, animationConnection_;
};
}
