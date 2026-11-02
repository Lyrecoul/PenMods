#pragma once
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ffmpeg_player {
// Only inspect box headers, using 64-bit offsets. Never scan media payloads.
// mvex identifies fragmented inputs even when an initial moov has sample tables.
inline bool OrdinaryMp4(const char *path) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return false;
  struct Close { int fd; ~Close() { close(fd); } } closeFile{fd};
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 8) return false;
  const auto integer = [](const uint8_t *p, int n) {
    uint64_t value = 0;
    while (n--) value = (value << 8) | *p++;
    return value;
  };
  const auto box = [&](uint64_t offset, uint64_t end, uint64_t &length,
                       uint64_t &header, char *type) {
    uint8_t bytes[16];
    if (end - offset < 8 || pread(fd, bytes, 8, offset) != 8) return false;
    length = integer(bytes, 4);
    std::memcpy(type, bytes + 4, 4);
    header = 8;
    if (length == 1) {
      if (end - offset < 16 || pread(fd, bytes + 8, 8, offset + 8) != 8) return false;
      length = integer(bytes + 8, 8);
      header = 16;
    } else if (!length) length = end - offset;
    return length >= header && length <= end - offset;
  };
  bool moov = false, ftyp = false;
  unsigned headers = 0;
  const uint64_t size = st.st_size;
  for (uint64_t offset = 0; offset < size;) {
    uint64_t length, header;
    char type[4];
    if (++headers > 4096 || !box(offset, size, length, header, type)) return false;
    if (!std::memcmp(type, "moof", 4)) return false;
    if (!std::memcmp(type, "ftyp", 4)) ftyp = true;
    if (!std::memcmp(type, "moov", 4)) {
      moov = true;
      for (uint64_t child = offset + header; child < offset + length;) {
        uint64_t childLength, childHeader;
        if (++headers > 4096 || !box(child, offset + length, childLength, childHeader, type)) return false;
        if (!std::memcmp(type, "mvex", 4)) return false;
        child += childLength;
      }
    }
    offset += length;
  }
  return moov && ftyp;
}
}
