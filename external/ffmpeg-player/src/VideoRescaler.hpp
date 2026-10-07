#ifndef VIDEO_RESCALER_HPP
#define VIDEO_RESCALER_HPP

#include "FFmpegCore.hpp"

namespace ffmpeg_player {

class VideoRescaler {
private:
  struct SwsContext *sws_ctx = nullptr;
  int dst_w, dst_h;
  AVPixelFormat dst_pix_fmt;

public:
  VideoRescaler(int width, int height, AVPixelFormat pix_fmt = AV_PIX_FMT_RGB24)
      : dst_w(width), dst_h(height), dst_pix_fmt(pix_fmt) {}

  ~VideoRescaler() {
    if (sws_ctx)
      sws_freeContext(sws_ctx);
  }

  // 转换图像
  // dst_data: 预先分配好的内存 buffer
  inline bool Rescale(AVFrame *src_frame, uint8_t *dst_data, int dst_linesize) {
    sws_ctx = sws_getCachedContext(sws_ctx, src_frame->width, src_frame->height,
                                   (AVPixelFormat)src_frame->format, dst_w,
                                   dst_h, dst_pix_fmt, SWS_BILINEAR, nullptr,
                                   nullptr, nullptr);

    if (!sws_ctx)
      return false;

    uint8_t *dest[4] = {dst_data, nullptr, nullptr, nullptr};
    int dest_linesizes[4] = {dst_linesize, 0, 0, 0};

    int ret = sws_scale(sws_ctx, src_frame->data, src_frame->linesize, 0,
                        src_frame->height, dest, dest_linesizes);

    return ret > 0;
  }
};

} // namespace ffmpeg_player
#endif