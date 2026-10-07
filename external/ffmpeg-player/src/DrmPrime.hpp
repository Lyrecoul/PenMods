#ifndef DRM_PRIME_HPP
#define DRM_PRIME_HPP

// ============================================================
// DRM PRIME 帧工具（h264_rkmpp 等硬解输出 AV_PIX_FMT_DRM_PRIME）
//
// rkmpp 输出的帧里 data[0] 是 AVDRMFrameDescriptor，真正的像素在
// dma-buf 中（NV12，带 hor_stride/ver_stride 对齐）。这里负责：
//   1. 解析描述符得到 NV12 两个平面的 fd / offset / pitch
//   2. 需要 CPU 访问时 mmap 映射（零拷贝渲染不可用时的回退路径）
// ============================================================

#include <cstddef>
#include <cstdint>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

#if defined(__has_include)
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#endif
#endif

#ifndef DMA_BUF_IOCTL_SYNC
struct dma_buf_sync {
  uint64_t flags;
};
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)
#endif

namespace ffmpeg_player {
namespace drm {

constexpr uint32_t kFourccNV12 = static_cast<uint32_t>('N') |
                                 (static_cast<uint32_t>('V') << 8) |
                                 (static_cast<uint32_t>('1') << 16) |
                                 (static_cast<uint32_t>('2') << 24);

struct Nv12Layout {
  int fd[2] = {-1, -1};
  size_t object_size[2] = {0, 0};
  ptrdiff_t offset[2] = {0, 0};
  ptrdiff_t pitch[2] = {0, 0};
  int width = 0;
  int height = 0;
};

inline const AVDRMFrameDescriptor *Descriptor(const AVFrame *frame) {
  if (!frame || frame->format != AV_PIX_FMT_DRM_PRIME || !frame->data[0])
    return nullptr;
  return reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
}

// 只接受单层 NV12（rkmpp 8bit 输出的布局），10bit(NA12) 等返回 false
inline bool GetNv12Layout(const AVFrame *frame, Nv12Layout &out) {
  const AVDRMFrameDescriptor *desc = Descriptor(frame);
  if (!desc || desc->nb_layers < 1)
    return false;

  const AVDRMLayerDescriptor &layer = desc->layers[0];
  if (layer.format != kFourccNV12 || layer.nb_planes < 2)
    return false;

  for (int i = 0; i < 2; ++i) {
    const int obj = layer.planes[i].object_index;
    if (obj < 0 || obj >= desc->nb_objects || desc->objects[obj].fd < 0)
      return false;
    out.fd[i] = desc->objects[obj].fd;
    out.object_size[i] = desc->objects[obj].size;
    out.offset[i] = layer.planes[i].offset;
    out.pitch[i] = layer.planes[i].pitch;
  }
  out.width = frame->width;
  out.height = frame->height;
  return out.pitch[0] >= out.width && out.pitch[1] >= out.width &&
         out.width > 0 && out.height > 0;
}

inline bool IsNv12(const AVFrame *frame) {
  Nv12Layout layout;
  return GetNv12Layout(frame, layout);
}

// ─── dma-buf 的 CPU 只读映射（RAII） ─────────────────────
// 注意：MPP 的 ION 缓冲通常是非 cache 映射，CPU 读取较慢，
// 仅作为 EGL 零拷贝不可用时的回退。
class Nv12Mapping {
public:
  explicit Nv12Mapping(const Nv12Layout &layout) {
    const size_t rows[2] = {static_cast<size_t>(layout.height),
                            static_cast<size_t>((layout.height + 1) / 2)};
    for (int i = 0; i < 2; ++i) {
      const size_t need = static_cast<size_t>(layout.offset[i]) +
                          static_cast<size_t>(layout.pitch[i]) * rows[i];
      const uint8_t *base = nullptr;
      for (int m = 0; m < map_count_; ++m) {
        if (maps_[m].fd == layout.fd[i] && maps_[m].size >= need)
          base = maps_[m].addr;
      }
      if (!base) {
        base = Map(layout.fd[i], layout.object_size[i], need);
        if (!base)
          return;
      }
      data[i] = base + layout.offset[i];
      linesize[i] = static_cast<int>(layout.pitch[i]);
    }
    ok_ = true;
  }

  ~Nv12Mapping() {
    for (int m = 0; m < map_count_; ++m) {
      Sync(maps_[m].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
      munmap(const_cast<uint8_t *>(maps_[m].addr), maps_[m].size);
    }
  }

  Nv12Mapping(const Nv12Mapping &) = delete;
  Nv12Mapping &operator=(const Nv12Mapping &) = delete;

  bool ok() const { return ok_; }

  const uint8_t *data[2] = {nullptr, nullptr};
  int linesize[2] = {0, 0};

private:
  struct MapEntry {
    int fd = -1;
    const uint8_t *addr = nullptr;
    size_t size = 0;
  };

  const uint8_t *Map(int fd, size_t size, size_t need) {
    if (map_count_ >= 2)
      return nullptr;
    if (size == 0) {
      const off_t end = lseek(fd, 0, SEEK_END);
      size = end > 0 ? static_cast<size_t>(end) : 0;
    }
    if (size < need)
      return nullptr;
    void *addr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
      return nullptr;
    // 内核 < 4.6 没有此 ioctl，失败可忽略
    Sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    maps_[map_count_++] = {fd, static_cast<const uint8_t *>(addr), size};
    return static_cast<const uint8_t *>(addr);
  }

  static void Sync(int fd, uint64_t flags) {
    struct dma_buf_sync sync = {flags};
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
  }

  MapEntry maps_[2];
  int map_count_ = 0;
  bool ok_ = false;
};

} // namespace drm
} // namespace ffmpeg_player

#endif
