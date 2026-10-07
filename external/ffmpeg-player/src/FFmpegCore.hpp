#ifndef FFMPEG_CORE_HPP
#define FFMPEG_CORE_HPP

#include "Logger.hpp"
#include <string>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace ffmpeg_player {

class FFmpegCore {
public:
  // 在 FFmpegCore 类中增加
  static void FFmpegLogCallback(void *ptr, int level, const char *fmt,
                                va_list vl) {
    char line[1024];
    thread_local int print_prefix = 1;
    av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);

    // 根据 FFmpeg 的级别映射到 glog
    if (level <= AV_LOG_ERROR) {
      LOG(ERROR) << "[FFmpeg] " << line;
    } else if (level <= AV_LOG_WARNING) {
      LOG(WARNING) << "[FFmpeg] " << line;
    } else if (level <= AV_LOG_INFO) {
      LOG(INFO) << "[FFmpeg] " << line;
    } else {
      // VLOG(1) << "[FFmpeg] " << line; // 调试级别
    }
  }

  static void Init() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
      // FFmpeg 3.4.8 仍需要这些注册函数
      // 4.0 以后这些被弃用，直接自动注册
      av_register_all();
      avcodec_register_all();
      avfilter_register_all();
      avformat_network_init();
      av_log_set_callback(FFmpegLogCallback);
      LOG(INFO) << "FFmpeg Core Initialized (Version: " << LIBAVFORMAT_IDENT
                << ")\n";
    });
  }

  // 辅助函数：将 FFmpeg 错误码转为字符串
  static std::string ErrorToString(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, errnum);
    return std::string(buf);
  }
};
} // namespace ffmpeg_player

#endif
