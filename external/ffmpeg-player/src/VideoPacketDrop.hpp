#pragma once

#include "FFmpegCore.hpp"
#include <cstddef>
#include <cstdint>

namespace ffmpeg_player {

// H.264 nal_ref_idc, rather than B-picture type, determines whether later
// pictures may reference a slice. Reject anything we cannot prove disposable.
inline bool IsNonReferenceVideoPacket(const AVPacket *packet,
                                     const AVCodecContext *context) {
  if (!packet || !context || (context->codec_id != AV_CODEC_ID_H264 && context->codec_id != AV_CODEC_ID_HEVC) ||
      !packet->data || packet->size <= 0 || packet->side_data_elems ||
      (packet->flags & (AV_PKT_FLAG_KEY | AV_PKT_FLAG_CORRUPT)))
    return false;

  bool has_slice = false;
  const bool hevc = context->codec_id == AV_CODEC_ID_HEVC;
  const auto disposable_nal = [&](const uint8_t *data, size_t size) {
    if (size < 2 || (data[0] & 0x80)) return false;
    if (hevc) {
      // Only base-layer, single-slice *_N access units. Reject multilayer,
      // multiple pictures/slices, parameter sets, SEI and reserved NAL types.
      if (size < 3 || (data[0] & 1) || (data[1] & 0xf8) || !(data[1] & 7)) return false;
      const unsigned type = (data[0] >> 1) & 0x3f;
      if (type <= 8 && !(type & 1) && (data[2] & 0x80) && !has_slice) {
        has_slice = true;
        return true;
      }
      return type == 35 || type == 38; // AUD / filler only
    }
    const int type = data[0] & 0x1f;
    if (type == 1 && !(data[0] & 0x60)) {
      has_slice = true;
      return true;
    }
    // AUD/filler have no persistent state. Preserve SPS/PPS, SEI, end markers,
    // data partitions and extension NALs even when accompanied by a B picture.
    return type == 9 || type == 12;
  };

  const uint8_t *data = packet->data;
  const size_t size = static_cast<size_t>(packet->size);
  if (context->extradata && context->extradata_size >= (hevc ? 23 : 7) &&
      context->extradata[0] == 1) {
    const size_t length_size = (context->extradata[hevc ? 21 : 4] & 3) + 1;
    size_t offset = 0;
    while (offset < size) {
      if (size - offset < length_size) return false;
      uint32_t length = 0;
      for (size_t i = 0; i < length_size; ++i)
        length = (length << 8) | data[offset++];
      if (!length || length > size - offset ||
          !disposable_nal(data + offset, length))
        return false;
      offset += length;
    }
    return has_slice;
  }

  const auto start_code = [&](size_t offset) {
    return offset + 3 <= size && data[offset] == 0 &&
           data[offset + 1] == 0 && data[offset + 2] == 1;
  };
  size_t offset = 0;
  while (!start_code(offset)) {
    if (offset >= size || data[offset] != 0) return false;
    ++offset;
  }
  for (;;) {
    const size_t begin = offset + 3;
    offset = begin;
    while (offset < size && !start_code(offset)) ++offset;
    if (!disposable_nal(data + begin, offset - begin)) return false;
    if (offset == size) return has_slice;
  }
}

} // namespace ffmpeg_player
