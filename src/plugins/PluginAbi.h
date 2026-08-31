#pragma once

#include <cstddef>
#include <cstdint>

class Activity;
class GfxRenderer;
class MappedInputManager;

namespace crosspoint_plugin {

constexpr uint32_t ABI_VERSION = 2;
constexpr char ROOT_PATH[] = "/apps/crosspoint-plugins";
constexpr char MANAGER_PATH[] = "/apps/crosspoint-plugins/manager.so";
constexpr char CREATE_SYMBOL[] = "crosspoint_plugin_create";
constexpr char ABI_SYMBOL[] = "crosspoint_plugin_abi";
constexpr char TRAILER_MAGIC[] = "X4PLUG01";
constexpr uint32_t TRAILER_FORMAT = 1;
constexpr size_t SHA256_BYTES = 32;
constexpr size_t TRAILER_BYTES = 8 + 4 + 4 + 4 + SHA256_BYTES;
constexpr size_t MAX_ELF_BYTES = 2 * 1024 * 1024;

using CreateActivity = Activity* (*)(GfxRenderer*, MappedInputManager*);
using ReadAbi = uint32_t (*)();

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
