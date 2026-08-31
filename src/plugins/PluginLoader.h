#pragma once

#include <esp_elf.h>

#include <cstdint>
#include <memory>

class Activity;
class GfxRenderer;
class MappedInputManager;

class PluginLoader final {
 public:
  enum class Error : uint8_t { NONE, MISSING, INCOMPATIBLE, CORRUPT, OUT_OF_MEMORY, LOAD_FAILED };

  std::unique_ptr<Activity> createManager(GfxRenderer& renderer, MappedInputManager& mappedInput);
  Activity* createChild(const char* moduleName, GfxRenderer& renderer, MappedInputManager& mappedInput);

  Activity* managerRoot() const { return manager_.rootActivity; }
  Activity* childRoot() const { return child_.rootActivity; }
  Error managerError() const { return manager_.lastError; }

  void unloadManager();
  void unloadChild();

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

  bool load(Module& module, const char* path);
  std::unique_ptr<Activity> create(Module& module, const char* path, GfxRenderer& renderer,
                                   MappedInputManager& mappedInput);
  void* findSymbol(Module& module, const char* name);
  void unload(Module& module);
};

#define PLUGIN_LOADER PluginLoader::getInstance()
