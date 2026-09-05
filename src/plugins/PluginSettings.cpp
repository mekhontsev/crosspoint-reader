#include "PluginAbi.h"

#if defined(ENABLE_PLUGINS) && ENABLE_PLUGINS

#include <nvs.h>

#include <array>
#include <cstring>

namespace {

constexpr char SETTINGS_NAMESPACE[] = "plugins";

bool validSettingsId(const char* id) {
  if (!id || !id[0]) return false;
  for (size_t index = 0; index <= crosspoint_plugin::SETTINGS_ID_MAX_BYTES; ++index) {
    const char value = id[index];
    if (value == '\0') return true;
    if (index == crosspoint_plugin::SETTINGS_ID_MAX_BYTES ||
        !((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-')) {
      return false;
    }
  }
  return false;
}

class SettingsHandle {
 public:
  nvs_handle_t value = 0;
  bool opened = false;
  ~SettingsHandle() {
    if (opened) nvs_close(value);
  }
  bool open(const nvs_open_mode_t mode) {
    opened = nvs_open(SETTINGS_NAMESPACE, mode, &value) == ESP_OK;
    return opened;
  }
};

}  // namespace

extern "C" uint8_t crosspoint_plugin_settings_read_v5(const char* id, uint8_t* data, const size_t capacity,
                                                      size_t* length) {
  if (!length) return false;
  *length = 0;
  if (!validSettingsId(id) || !data || capacity == 0 || capacity > crosspoint_plugin::SETTINGS_MAX_BYTES) {
    return false;
  }
  SettingsHandle handle;
  if (!handle.open(NVS_READONLY)) return false;
  size_t bytes = capacity;
  if (nvs_get_blob(handle.value, id, data, &bytes) != ESP_OK || bytes == 0 || bytes > capacity) return false;
  *length = bytes;
  return true;
}

extern "C" uint8_t crosspoint_plugin_settings_write_v5(const char* id, const uint8_t* data, const size_t length) {
  if (!validSettingsId(id) || !data || length == 0 || length > crosspoint_plugin::SETTINGS_MAX_BYTES) return false;
  SettingsHandle handle;
  if (!handle.open(NVS_READWRITE)) return false;
  std::array<uint8_t, crosspoint_plugin::SETTINGS_MAX_BYTES> previous{};
  size_t bytes = previous.size();
  const esp_err_t result = nvs_get_blob(handle.value, id, previous.data(), &bytes);
  if (result == ESP_OK && bytes == length && std::memcmp(previous.data(), data, length) == 0) return true;
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND && result != ESP_ERR_NVS_INVALID_LENGTH) return false;
  return nvs_set_blob(handle.value, id, data, length) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}

#endif
