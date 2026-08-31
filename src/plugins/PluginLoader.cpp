#include "PluginLoader.h"

#if defined(ENABLE_PLUGINS) && ENABLE_PLUGINS

#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "activities/Activity.h"
#include "plugins/PluginAbi.h"
#include "plugins/PluginHostSymbols.h"
#include "private/elf_platform.h"
#include "private/elf_symbol.h"
#include "private/elf_types.h"

namespace {

constexpr size_t TRAILER_FORMAT_OFFSET = 8;
constexpr size_t TRAILER_ABI_OFFSET = 12;
constexpr size_t TRAILER_ELF_SIZE_OFFSET = 16;
constexpr size_t TRAILER_HASH_OFFSET = 20;
constexpr size_t MODULE_NAME_MAX = 32;

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

bool rangeWithin(const size_t offset, const size_t length, const size_t total) {
  return offset <= total && length <= total - offset;
}

bool validateElfImage(const uint8_t* payload, const size_t size) {
  if (!payload || size < sizeof(elf32_hdr_t)) return false;

  elf32_hdr_t header{};
  std::memcpy(&header, payload, sizeof(header));
  if (header.ident[0] != 0x7f || header.ident[1] != 'E' || header.ident[2] != 'L' || header.ident[3] != 'F' ||
      header.ident[4] != 1 || header.ident[5] != 1 || header.type != 3 || header.machine != 94 ||
      header.ehsize != sizeof(elf32_hdr_t) || header.shentsize != sizeof(elf32_shdr_t) || header.shnum == 0 ||
      header.shstrndx >= header.shnum ||
      !rangeWithin(header.shoff, static_cast<size_t>(header.shnum) * sizeof(elf32_shdr_t), size)) {
    return false;
  }

  for (uint16_t index = 0; index < header.shnum; ++index) {
    elf32_shdr_t section{};
    std::memcpy(&section, payload + header.shoff + static_cast<size_t>(index) * sizeof(section), sizeof(section));
    if (section.type != SHT_NOBITS && !rangeWithin(section.offset, section.size, size)) return false;
    if ((section.type == SHT_RELA || section.type == SHT_SYMTAB || section.type == SHT_SYNSYM) &&
        section.link >= header.shnum) {
      return false;
    }
    if (section.entsize != 0 && section.size % section.entsize != 0) return false;
  }
  return true;
}

uintptr_t resolvePluginSymbol(const char* name) {
  const uintptr_t address = resolvePluginHostSymbol(name);
  return address != 0 ? address : elf_find_sym_default(name);
}

bool validModuleName(const char* name) {
  if (!name || name[0] == '\0') return false;
  size_t length = 0;
  for (; name[length] != '\0'; ++length) {
    if (length >= MODULE_NAME_MAX) return false;
    const char value = name[length];
    if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-' || value == '_')) {
      return false;
    }
  }
  return length > 0;
}

struct PsramDeleter {
  void operator()(uint8_t* pointer) const {
    if (pointer) heap_caps_free(pointer);
  }
};

}  // namespace

PluginLoader& PluginLoader::getInstance() {
  static PluginLoader instance;
  return instance;
}

bool PluginLoader::load(Module& module, const char* path) {
  if (module.loaded) return true;
  module.lastError = Error::NONE;

  HalFile file;
  if (!Storage.openFileForRead("PLUGIN", path, file) || !file) {
    LOG_ERR("PLUGIN", "Missing module: %s", path);
    module.lastError = Error::MISSING;
    return false;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize <= crosspoint_plugin::TRAILER_BYTES ||
      fileSize > crosspoint_plugin::MAX_ELF_BYTES + crosspoint_plugin::TRAILER_BYTES) {
    LOG_ERR("PLUGIN", "Invalid module size: %u", static_cast<unsigned>(fileSize));
    module.lastError = Error::CORRUPT;
    return false;
  }

  std::array<uint8_t, crosspoint_plugin::TRAILER_BYTES> trailer{};
  if (!file.seek(fileSize - trailer.size()) || file.read(trailer.data(), trailer.size()) != trailer.size()) {
    LOG_ERR("PLUGIN", "Unable to read module trailer");
    module.lastError = Error::CORRUPT;
    return false;
  }

  const size_t elfSize = readLe32(trailer.data() + TRAILER_ELF_SIZE_OFFSET);
  if (std::memcmp(trailer.data(), crosspoint_plugin::TRAILER_MAGIC, 8) != 0 ||
      readLe32(trailer.data() + TRAILER_FORMAT_OFFSET) != crosspoint_plugin::TRAILER_FORMAT ||
      readLe32(trailer.data() + TRAILER_ABI_OFFSET) != crosspoint_plugin::ABI_VERSION ||
      elfSize != fileSize - trailer.size()) {
    LOG_ERR("PLUGIN", "Module is incompatible with this firmware: %s", path);
    module.lastError = Error::INCOMPATIBLE;
    return false;
  }

  // Relocation needs the complete ELF image at once. This cold-path buffer is
  // explicitly allocated in the X4 Pro's PSRAM and released immediately after
  // esp_elf_relocate has copied the loadable sections into executable PSRAM.
  std::unique_ptr<uint8_t, PsramDeleter> payload(
      static_cast<uint8_t*>(heap_caps_malloc(elfSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  if (!payload) {
    LOG_ERR("PLUGIN", "OOM: module payload (%u bytes)", static_cast<unsigned>(elfSize));
    module.lastError = Error::OUT_OF_MEMORY;
    return false;
  }

  if (!file.seek(0)) {
    module.lastError = Error::CORRUPT;
    return false;
  }
  mbedtls_sha256_context sha{};
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t position = 0;
  while (position < elfSize) {
    const size_t chunk = std::min<size_t>(4096, elfSize - position);
    const int count = file.read(payload.get() + position, chunk);
    if (count != static_cast<int>(chunk)) {
      mbedtls_sha256_free(&sha);
      LOG_ERR("PLUGIN", "Short module read at %u", static_cast<unsigned>(position));
      module.lastError = Error::CORRUPT;
      return false;
    }
    mbedtls_sha256_update(&sha, payload.get() + position, chunk);
    position += chunk;
  }
  std::array<uint8_t, crosspoint_plugin::SHA256_BYTES> digest{};
  mbedtls_sha256_finish(&sha, digest.data());
  mbedtls_sha256_free(&sha);
  if (std::memcmp(digest.data(), trailer.data() + TRAILER_HASH_OFFSET, digest.size()) != 0 ||
      !validateElfImage(payload.get(), elfSize)) {
    LOG_ERR("PLUGIN", "Module integrity validation failed: %s", path);
    module.lastError = Error::CORRUPT;
    return false;
  }

  elf_set_symbol_resolver(resolvePluginSymbol);
  if (esp_elf_init(&module.elf) != 0) {
    module.lastError = Error::LOAD_FAILED;
    return false;
  }
  module.initialized = true;
  if (esp_elf_relocate(&module.elf, payload.get()) != 0) {
    LOG_ERR("PLUGIN", "ELF relocation failed: %s", path);
    unload(module);
    module.lastError = Error::LOAD_FAILED;
    return false;
  }
  module.loaded = true;
  LOG_INF("PLUGIN", "Loaded %u-byte module into PSRAM: %s", static_cast<unsigned>(elfSize), path);
  return true;
}

void* PluginLoader::findSymbol(Module& module, const char* name) {
  if (!module.loaded || !name || !module.elf.symtab) return nullptr;
  for (uint16_t index = 0; index < module.elf.num; ++index) {
    if (module.elf.symtab[index].name && std::strcmp(module.elf.symtab[index].name, name) == 0) {
      uintptr_t address = reinterpret_cast<uintptr_t>(module.elf.symtab[index].addr);
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
      address = elf_remap_text(&module.elf, address);
#endif
      return reinterpret_cast<void*>(address);
    }
  }
  return nullptr;
}

std::unique_ptr<Activity> PluginLoader::create(Module& module, const char* path, GfxRenderer& renderer,
                                               MappedInputManager& mappedInput) {
  if (!load(module, path)) return nullptr;
  auto readAbi = reinterpret_cast<crosspoint_plugin::ReadAbi>(findSymbol(module, crosspoint_plugin::ABI_SYMBOL));
  auto create =
      reinterpret_cast<crosspoint_plugin::CreateActivity>(findSymbol(module, crosspoint_plugin::CREATE_SYMBOL));
  if (!readAbi || !create || readAbi() != crosspoint_plugin::ABI_VERSION) {
    LOG_ERR("PLUGIN", "Required module exports are missing: %s", path);
    module.lastError = Error::INCOMPATIBLE;
    unload(module);
    return nullptr;
  }
  Activity* activity = create(&renderer, &mappedInput);
  if (!activity) {
    LOG_ERR("PLUGIN", "Module could not allocate its root activity: %s", path);
    module.lastError = Error::OUT_OF_MEMORY;
    unload(module);
    return nullptr;
  }
  module.rootActivity = activity;
  return std::unique_ptr<Activity>(activity);
}

std::unique_ptr<Activity> PluginLoader::createManager(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return create(manager_, crosspoint_plugin::MANAGER_PATH, renderer, mappedInput);
}

Activity* PluginLoader::createChild(const char* moduleName, GfxRenderer& renderer,
                                    MappedInputManager& mappedInput) {
  if (!validModuleName(moduleName) || child_.rootActivity) return nullptr;
  std::array<char, 128> path{};
  const int length = std::snprintf(path.data(), path.size(), "%s/%s.so", crosspoint_plugin::ROOT_PATH, moduleName);
  if (length <= 0 || static_cast<size_t>(length) >= path.size()) return nullptr;
  auto activity = create(child_, path.data(), renderer, mappedInput);
  return activity.release();
}

void PluginLoader::unload(Module& module) {
  if (module.initialized) esp_elf_deinit(&module.elf);
  module.elf = {};
  module.rootActivity = nullptr;
  module.initialized = false;
  module.loaded = false;
  elf_reset_symbol_resolver();
}

void PluginLoader::unloadManager() { unload(manager_); }

void PluginLoader::unloadChild() { unload(child_); }

extern "C" Activity* crosspoint_plugin_create_child(const char* moduleName, GfxRenderer* renderer,
                                                     MappedInputManager* mappedInput) {
  if (!renderer || !mappedInput) return nullptr;
  return PLUGIN_LOADER.createChild(moduleName, *renderer, *mappedInput);
}

#endif
