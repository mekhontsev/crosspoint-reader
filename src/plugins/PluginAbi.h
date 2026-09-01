#pragma once

#include <cstddef>
#include <cstdint>

class Activity;
class GfxRenderer;
class MappedInputManager;

namespace crosspoint_plugin {

constexpr uint32_t ABI_VERSION = 3;
constexpr char ROOT_PATH[] = "/plugins";
constexpr char MANAGER_PATH[] = "/plugins/manager.so";
constexpr char CREATE_SYMBOL[] = "crosspoint_plugin_create";
constexpr char ABI_SYMBOL[] = "crosspoint_plugin_abi";
constexpr char METADATA_SECTION[] = ".crosspoint.plugin";
constexpr char TRAILER_MAGIC[] = "X4PLUG01";
constexpr uint32_t TRAILER_FORMAT = 1;
constexpr size_t SHA256_BYTES = 32;
constexpr size_t TRAILER_BYTES = 8 + 4 + 4 + 4 + SHA256_BYTES;
constexpr size_t MAX_ELF_BYTES = 2 * 1024 * 1024;
constexpr size_t MODULE_NAME_BYTES = 33;
constexpr size_t MODULE_TITLE_BYTES = 48;
constexpr size_t MODULE_VERSION_BYTES = 16;
constexpr size_t MAX_LISTED_MODULES = 12;

using CreateActivity = Activity* (*)(GfxRenderer*, MappedInputManager*);
using ReadAbi = uint32_t (*)();

struct PluginDescriptorV3 {
  char title[MODULE_TITLE_BYTES]{};
  char version[MODULE_VERSION_BYTES]{};
  int16_t order = 0;
  uint16_t flags = 0;
};

struct PluginInfoV3 {
  char module[MODULE_NAME_BYTES]{};
  PluginDescriptorV3 descriptor{};
};

enum class UpdateStatusV3 : uint8_t { READY = 1, COMPLETE = 2, ERROR = 3 };

constexpr uint32_t KEYBOARD_FLAG_HEADER_TOGGLE = 1U << 0U;
constexpr uint32_t KEYBOARD_FLAG_SYSTEM_LANGUAGE = 1U << 1U;

}  // namespace crosspoint_plugin

// Stable host entry point available to manager.so. The returned activity is
// owned by the caller and keeps its corresponding module loaded until it is
// destroyed.
extern "C" Activity* crosspoint_plugin_create_child(const char* moduleName, GfxRenderer* renderer,
                                                    MappedInputManager* mappedInput);

// Firmware owns the complete keyboard activity and its result. The plugin only
// opens it and retrieves a byte buffer after the activity has returned.
extern "C" uint8_t crosspoint_plugin_open_text_keyboard_v2(const char* title, size_t maxLength, uint32_t flags,
                                                           GfxRenderer* renderer, MappedInputManager* mappedInput);
extern "C" uint8_t crosspoint_plugin_take_text_keyboard_result_v2(char* text, size_t capacity, size_t* length,
                                                                  uint8_t* cancelled);
extern "C" uint8_t crosspoint_plugin_send_terminal_command_v2(const char* text, size_t length);

// ABI 3: dynamic child discovery and direct streaming installation. The
// manager module itself is never returned by the list and cannot be replaced.
extern "C" size_t crosspoint_plugin_list_v3(crosspoint_plugin::PluginInfoV3* modules, size_t capacity);
extern "C" uint8_t crosspoint_plugin_install_begin_v3(const char* module, uint32_t bytes,
                                                      const uint8_t sha256[crosspoint_plugin::SHA256_BYTES]);
extern "C" uint8_t crosspoint_plugin_install_write_v3(uint32_t offset, const uint8_t* data, size_t length);
extern "C" uint8_t crosspoint_plugin_install_finish_v3();
extern "C" void crosspoint_plugin_install_abort_v3();
extern "C" uint8_t crosspoint_plugin_send_update_hello_v3();
extern "C" uint8_t crosspoint_plugin_send_update_status_v3(crosspoint_plugin::UpdateStatusV3 status, uint32_t value);
