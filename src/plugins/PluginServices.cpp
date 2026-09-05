#include "PluginAbi.h"

#if defined(ENABLE_PLUGINS) && ENABLE_PLUGINS

#include <HalStorage.h>
#include <Logging.h>

#include <array>
#include <cstring>

// Internal bounded logging bridge; plugin callers use the ABI entry point below.
size_t copyLastLogs(uint8_t* output, size_t capacity);

namespace {
struct Provider {
  std::array<char, 16> id{};
  crosspoint_plugin::StateProvider callback = nullptr;
  void* context = nullptr;
};
std::array<Provider, 4> providers{};

bool validId(const char* id) {
  if (!id || !id[0]) return false;
  for (size_t i = 0; i < 16; ++i) {
    const char c = id[i];
    if (c == '\0') return true;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return false;
}
}  // namespace

extern "C" size_t crosspoint_plugin_logs_copy_v5(uint8_t* output, const size_t capacity) {
  if (!output || capacity != 4096) return 0;
  return copyLastLogs(output, capacity);
}

extern "C" uint8_t crosspoint_plugin_state_register_v5(const char* id, crosspoint_plugin::StateProvider callback,
                                                       void* context) {
  if (!validId(id) || !callback || !context) return false;
  for (auto& entry : providers) {
    if (entry.callback && std::strcmp(entry.id.data(), id) == 0) {
      return entry.context == context && entry.callback == callback;
    }
  }
  for (auto& entry : providers) {
    if (entry.callback) continue;
    std::strcpy(entry.id.data(), id);
    entry.callback = callback;
    entry.context = context;
    return true;
  }
  return false;
}

extern "C" void crosspoint_plugin_state_unregister_v5(const char* id, void* context) {
  if (!validId(id)) return;
  for (auto& entry : providers) {
    if (entry.context == context && std::strcmp(entry.id.data(), id) == 0) entry = {};
  }
}

extern "C" size_t crosspoint_plugin_state_call_v5(const char* id, const uint8_t* request, const size_t length,
                                                  uint8_t* response, const size_t capacity) {
  if (!validId(id) || !request || !length || length > 239 || !response || !capacity || capacity > 239) return 0;
  for (auto& entry : providers) {
    if (entry.callback && std::strcmp(entry.id.data(), id) == 0) {
      const size_t bytes = entry.callback(entry.context, request, length, response, capacity);
      return bytes <= capacity ? bytes : 0;
    }
  }
  return 0;
}

extern "C" uint8_t crosspoint_plugin_file_read_v5(const char* path, const uint32_t offset, uint8_t* data,
                                                  const size_t capacity, size_t* length, uint32_t* total) {
  if (!length || !total) return false;
  *length = 0;
  *total = 0;
  if (!path || path[0] != '/' || !data || !capacity || capacity > 224) return false;
  size_t size = 0;
  for (; size < 128 && path[size]; ++size) {
    if (static_cast<uint8_t>(path[size]) < 32 || path[size] == '\\') return false;
  }
  if (size == 0 || size == 128) return false;
  for (size_t start = 1; start < size;) {
    size_t end = start;
    while (end < size && path[end] != '/') ++end;
    const size_t bytes = end - start;
    if ((bytes == 1 && path[start] == '.') || (bytes == 2 && path[start] == '.' && path[start + 1] == '.'))
      return false;
    start = end + 1;
  }
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file || file.isDirectory() || file.fileSize64() > UINT32_MAX) return false;
  *total = static_cast<uint32_t>(file.fileSize64());
  if (offset > *total || !file.seek(offset)) return false;
  const int bytes = file.read(data, capacity);
  if (bytes < 0) return false;
  *length = static_cast<size_t>(bytes);
  return true;
}

#endif
