#pragma once
#include "SubtitleDocument.hpp"
#include <QImage>
#include <QRect>
#include <ass/ass.h>
#include <cstdarg>
#include <cstdio>

namespace ffmpeg_player::subtitles {
struct Bitmap {
    QImage image;
    QRect bounds;
    bool changed = true;
    QString error;
};

// 每个实例只在一个渲染线程内使用。视频帧不参与字幕的光栅化或合成。
class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer() {
        if (track_) ass_free_track(track_);
        if (renderer_) ass_renderer_done(renderer_);
        if (library_) ass_library_done(library_);
    }
    Bitmap render(const DocumentPtr& document, const QString& font, double scale,
                  QSize frame, QSize storage, qint64 timestamp) {
        Bitmap output;
        if (!document || frame.isEmpty()) return output;
        if (font.isEmpty() || !QFileInfo(font).isReadable()) {
            font_.clear(); // 叠加层会清掉旧纹理，恢复字体时必须重新输出像素。
            output.error = "No readable subtitle font; set subtitleFontFile";
            return output;
        }
        if (!library_) {
            library_ = ass_library_init();
            if (library_) {
                ass_set_message_cb(library_, log, nullptr);
                ass_set_extract_fonts(library_, 0);
                renderer_ = ass_renderer_init(library_);
            }
        }
        if (!renderer_) { output.error = "Cannot initialize libass"; return output; }
        const bool changed = document != document_ || font != font_ || scale != scale_ ||
                             frame != frame_ || storage != storage_;
        if (font != font_) {
            const QByteArray path = QFile::encodeName(font);
            ass_set_fonts(renderer_, path.constData(), "sans-serif", ASS_FONTPROVIDER_NONE, nullptr, 1);
            ass_set_cache_limits(renderer_, 1000, 16);
            font_ = font;
        }
        if (document != document_) {
            if (track_) ass_free_track(track_);
            QByteArray data = document->ass;
            track_ = ass_read_memory(library_, data.data(), size_t(data.size()), nullptr);
            document_ = document;
        }
        if (!track_) { output.error = "Cannot parse ASS subtitle"; return output; }
        if (changed) {
            ass_set_frame_size(renderer_, frame.width(), frame.height());
            ass_set_storage_size(renderer_, storage.width(), storage.height());
            ass_set_font_scale(renderer_, scale);
            frame_ = frame;
            storage_ = storage;
            scale_ = scale;
        }
        int change = 0;
        ASS_Image* images = ass_render_frame(renderer_, track_, timestamp, &change);
        output.changed = changed || change != 0;
        if (!output.changed) return output;
        const QRect viewport(QPoint(0, 0), frame);
        QRect bounds;
        for (auto* part = images; part; part = part->next) {
            if (part->w <= 0 || part->h <= 0 || !part->bitmap) continue;
            bounds = bounds.united(QRect(part->dst_x, part->dst_y, part->w, part->h).intersected(viewport));
        }
        if (bounds.isEmpty()) return output;
        output.bounds = bounds;
        output.image = QImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
        if (output.image.isNull()) { output.error = "Cannot allocate subtitle bitmap"; return output; }
        output.image.fill(Qt::transparent);
        for (auto* part = images; part; part = part->next) {
            if (!part->bitmap || part->w <= 0 || part->h <= 0) continue;
            const QRect clipped = QRect(part->dst_x, part->dst_y, part->w, part->h).intersected(bounds);
            const unsigned r = part->color >> 24;
            const unsigned g = (part->color >> 16) & 255;
            const unsigned b = (part->color >> 8) & 255;
            const unsigned opacity = 255 - (part->color & 255);
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
                const auto* mask = part->bitmap + (y - part->dst_y) * part->stride;
                auto* dest = reinterpret_cast<QRgb*>(output.image.scanLine(y - bounds.top()));
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    const unsigned a = (mask[x - part->dst_x] * opacity + 127) / 255;
                    if (!a) continue;
                    QRgb& pixel = dest[x - bounds.left()];
                    const unsigned inv = 255 - a;
                    pixel = qRgba((r * a + qRed(pixel) * inv + 127) / 255,
                                  (g * a + qGreen(pixel) * inv + 127) / 255,
                                  (b * a + qBlue(pixel) * inv + 127) / 255,
                                  a + (qAlpha(pixel) * inv + 127) / 255);
                }
            }
        }
        return output;
    }
private:
    static void log(int level, const char* format, va_list arguments, void*) {
        if (level > 2) return;
        std::fputs("[libass] ", stderr);
        std::vfprintf(stderr, format, arguments);
        std::fputc('\n', stderr);
    }
    ASS_Library* library_ = nullptr;
    ASS_Renderer* renderer_ = nullptr;
    ASS_Track* track_ = nullptr;
    DocumentPtr document_;
    QString font_;
    double scale_ = 0;
    QSize frame_, storage_;
};
}
