#pragma once
#include <fcntl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
struct HalFile {
  bool valid = true;
  size_t offset = 0;
  operator bool() const { return valid; }
  bool isDirectory() const { return false; }
  uint64_t fileSize64() const { return 6; }
  bool seek(size_t value) {
    offset = value;
    return value <= 6;
  }
  int read(void* data, size_t capacity) {
    const size_t length = std::min(capacity, 6 - offset);
    std::memcpy(data, &"abcdef"[offset], length);
    return static_cast<int>(length);
  }
};
struct TestStorage {
  int opens = 0;
  int lastMode = -1;
  HalFile open(const char* path, int mode) {
    ++opens;
    lastMode = mode;
    return {std::strcmp(path, "/missing") != 0, 0};
  }
};
inline TestStorage Storage;
