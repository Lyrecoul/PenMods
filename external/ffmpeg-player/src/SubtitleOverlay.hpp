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
        if (!document_ || width() <= 0 || height() <= 0) { delete node; return nullptr; }
        if (!node) node = new Node;
        const qreal dpr = window()->effectiveDevicePixelRatio();
        QSize frame(std::max(1, int(std::ceil(width() * dpr))), std::max(1, int(std::ceil(height() * dpr))));
        if (frame.width() > 2048 || frame.height() > 2048) frame.scale(2048, 2048, Qt::KeepAspectRatio);
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
                    const bool added = !node->quad;
                    if (!node->quad) {
                        node->quad = new QSGSimpleTextureNode;
                        node->quad->setOwnsTexture(false);
                        node->quad->setFiltering(QSGTexture::Linear);
                    }
                    auto* previous = node->ownedTexture;
                    node->quad->setTexture(texture);
                    node->ownedTexture = texture;
                    delete previous;
                    node->quad->setRect(QRectF(bitmap.bounds.x() * width() / frame.width(),
                                        bitmap.bounds.y() * height() / frame.height(),
                                        bitmap.bounds.width() * width() / frame.width(),
                                        bitmap.bounds.height() * height() / frame.height()));
                    // 软件场景图会在插入节点时立即读取材质，先初始化纹理再挂到树上。
                    if (added) node->appendChildNode(node->quad);
                }
            }
        }
        return node;
    }
private:
    class Node : public QSGNode {
    public:
        ~Node() override { clearBitmap(); }
        void clearBitmap() {
            if (quad) { removeChildNode(quad); delete quad; quad = nullptr; }
            delete ownedTexture;
            ownedTexture = nullptr;
        }
        Renderer renderer;
        QSGSimpleTextureNode* quad = nullptr;
        QSGTexture* ownedTexture = nullptr;
        QString lastError;
        quint64 errorRevision = 0;
    };
    DocumentPtr document_;
    QString font_;
    double scale_ = 1;
    qint64 time_ = -1;
    QSize storage_;
    quint64 revision_ = 0;
};
}
