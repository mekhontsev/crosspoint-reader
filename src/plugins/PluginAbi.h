#pragma once

#include <cstddef>
#include <cstdint>

class Activity;
class GfxRenderer;
class MappedInputManager;

namespace crosspoint_plugin {

constexpr uint32_t ABI_VERSION = 1;
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

}  // namespace crosspoint_plugin

// Stable host entry point available to manager.so. The returned activity is
// owned by the caller and keeps its corresponding module loaded until it is
// destroyed.
extern "C" Activity* crosspoint_plugin_create_child(const char* moduleName, GfxRenderer* renderer,
                                                     MappedInputManager* mappedInput);
