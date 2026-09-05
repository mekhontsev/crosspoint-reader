#pragma once

#include <esp_elf.h>

#include <array>
#include <cstdint>
#include <memory>

#include "plugins/PluginAbi.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

class PluginLoader final {
 public:
  enum class Error : uint8_t { NONE, MISSING, INCOMPATIBLE, CORRUPT, OUT_OF_MEMORY, LOAD_FAILED };

  std::unique_ptr<Activity> createManager(GfxRenderer& renderer, MappedInputManager& mappedInput);
  Activity* createChild(const char* moduleName, GfxRenderer& renderer, MappedInputManager& mappedInput);
  size_t listChildren(crosspoint_plugin::PluginInfoV3* modules, size_t capacity);
  bool beginInstall(const char* moduleName, uint32_t bytes, const uint8_t sha256[crosspoint_plugin::SHA256_BYTES]);
  bool writeInstall(uint32_t offset, const uint8_t* data, size_t length);
  bool finishInstall();
  void abortInstall();

  Activity* managerRoot() const { return manager_.rootActivity; }
  Activity* childRoot() const { return child_.rootActivity; }
  Error managerError() const { return manager_.lastError; }

  void unloadManager();
  void unloadChild();
  void serviceBackground();

  static PluginLoader& getInstance();

 private:
  struct Module {
    esp_elf_t elf{};
    Activity* rootActivity = nullptr;
    Error lastError = Error::NONE;
    bool initialized = false;
    bool loaded = false;
  };

  Module manager_;
  Module child_;
  Module service_;
  std::array<char, crosspoint_plugin::MODULE_NAME_BYTES> serviceName_{};
  std::array<uint8_t, crosspoint_plugin::PLUGIN_BLE_MAX_PACKET_BYTES> serviceResponse_{};
  size_t serviceResponseBytes_ = 0;
  uint32_t serviceResponseAt_ = 0;
  uint32_t serviceConnectionRevision_ = 0;
  crosspoint_plugin::ServiceRequest serviceRequest_ = nullptr;
  void unloadService();

  bool load(Module& module, const char* path);
  bool describeChild(const char* moduleName, crosspoint_plugin::PluginInfoV3* info);
  std::unique_ptr<Activity> create(Module& module, const char* path, GfxRenderer& renderer,
                                   MappedInputManager& mappedInput);
  void* findSymbol(Module& module, const char* name);
  void unload(Module& module);
};

#define PLUGIN_LOADER PluginLoader::getInstance()
