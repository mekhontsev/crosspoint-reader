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
#include <string>
#include <type_traits>
#include <utility>

#include "Memory.h"
#include "activities/Activity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "ble/PluginBleTransport.h"
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

static_assert(std::is_standard_layout_v<crosspoint_plugin::PluginDescriptorV3>);
static_assert(std::is_trivially_copyable_v<crosspoint_plugin::PluginDescriptorV3>);
static_assert(sizeof(crosspoint_plugin::PluginDescriptorV3) ==
              crosspoint_plugin::MODULE_TITLE_BYTES + crosspoint_plugin::MODULE_VERSION_BYTES + 4);

struct PluginInstallState {
  HalFile file;
  mbedtls_sha256_context sha{};
  std::array<uint8_t, crosspoint_plugin::SHA256_BYTES> expectedSha{};
  std::array<char, 128> path{};
  std::array<char, crosspoint_plugin::MODULE_NAME_BYTES> module{};
  uint32_t expectedBytes = 0;
  uint32_t writtenBytes = 0;
  bool shaInitialized = false;
  bool active = false;
};

PluginInstallState pluginInstall;

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

void clearPluginInstall() {
  if (pluginInstall.file) pluginInstall.file.close();
  if (pluginInstall.shaInitialized) mbedtls_sha256_free(&pluginInstall.sha);
  pluginInstall = {};
}

struct PsramDeleter {
  void operator()(uint8_t* pointer) const {
    if (pointer) heap_caps_free(pointer);
  }
};

struct ValidatedModuleImage {
  std::unique_ptr<uint8_t, PsramDeleter> payload;
  size_t elfSize = 0;
};

bool readValidatedModuleImage(const char* path, ValidatedModuleImage& image, PluginLoader::Error& error) {
  HalFile file;
  if (!Storage.openFileForRead("PLUGIN", path, file) || !file) {
    LOG_ERR("PLUGIN", "Missing module: %s", path);
    error = PluginLoader::Error::MISSING;
    return false;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize <= crosspoint_plugin::TRAILER_BYTES ||
      fileSize > crosspoint_plugin::MAX_ELF_BYTES + crosspoint_plugin::TRAILER_BYTES) {
    LOG_ERR("PLUGIN", "Invalid module size: %u", static_cast<unsigned>(fileSize));
    error = PluginLoader::Error::CORRUPT;
    return false;
  }

  std::array<uint8_t, crosspoint_plugin::TRAILER_BYTES> trailer{};
  if (!file.seek(fileSize - trailer.size()) || file.read(trailer.data(), trailer.size()) != trailer.size()) {
    LOG_ERR("PLUGIN", "Unable to read module trailer");
    error = PluginLoader::Error::CORRUPT;
    return false;
  }

  const size_t elfSize = readLe32(trailer.data() + TRAILER_ELF_SIZE_OFFSET);
  if (std::memcmp(trailer.data(), crosspoint_plugin::TRAILER_MAGIC, 8) != 0 ||
      readLe32(trailer.data() + TRAILER_FORMAT_OFFSET) != crosspoint_plugin::TRAILER_FORMAT ||
      readLe32(trailer.data() + TRAILER_ABI_OFFSET) != crosspoint_plugin::ABI_VERSION ||
      elfSize != fileSize - trailer.size()) {
    LOG_ERR("PLUGIN", "Module is incompatible with this firmware: %s", path);
    error = PluginLoader::Error::INCOMPATIBLE;
    return false;
  }

  image.payload.reset(static_cast<uint8_t*>(heap_caps_malloc(elfSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  if (!image.payload) {
    LOG_ERR("PLUGIN", "OOM: module payload (%u bytes)", static_cast<unsigned>(elfSize));
    error = PluginLoader::Error::OUT_OF_MEMORY;
    return false;
  }

  if (!file.seek(0)) {
    error = PluginLoader::Error::CORRUPT;
    return false;
  }
  mbedtls_sha256_context sha{};
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t position = 0;
  while (position < elfSize) {
    const size_t chunk = std::min<size_t>(4096, elfSize - position);
    const int count = file.read(image.payload.get() + position, chunk);
    if (count != static_cast<int>(chunk)) {
      mbedtls_sha256_free(&sha);
      LOG_ERR("PLUGIN", "Short module read at %u", static_cast<unsigned>(position));
      error = PluginLoader::Error::CORRUPT;
      return false;
    }
    mbedtls_sha256_update(&sha, image.payload.get() + position, chunk);
    position += chunk;
  }
  std::array<uint8_t, crosspoint_plugin::SHA256_BYTES> digest{};
  mbedtls_sha256_finish(&sha, digest.data());
  mbedtls_sha256_free(&sha);
  if (std::memcmp(digest.data(), trailer.data() + TRAILER_HASH_OFFSET, digest.size()) != 0 ||
      !validateElfImage(image.payload.get(), elfSize)) {
    LOG_ERR("PLUGIN", "Module integrity validation failed: %s", path);
    error = PluginLoader::Error::CORRUPT;
    return false;
  }

  image.elfSize = elfSize;
  error = PluginLoader::Error::NONE;
  return true;
}

bool readPluginMetadata(const ValidatedModuleImage& image, crosspoint_plugin::PluginDescriptorV3& descriptor) {
  if (!image.payload || image.elfSize < sizeof(elf32_hdr_t)) return false;
  elf32_hdr_t header{};
  std::memcpy(&header, image.payload.get(), sizeof(header));

  elf32_shdr_t stringSection{};
  std::memcpy(&stringSection,
              image.payload.get() + header.shoff + static_cast<size_t>(header.shstrndx) * sizeof(elf32_shdr_t),
              sizeof(stringSection));
  if (!rangeWithin(stringSection.offset, stringSection.size, image.elfSize)) return false;

  const char* names = reinterpret_cast<const char*>(image.payload.get() + stringSection.offset);
  for (uint16_t index = 0; index < header.shnum; ++index) {
    elf32_shdr_t section{};
    std::memcpy(&section, image.payload.get() + header.shoff + static_cast<size_t>(index) * sizeof(section),
                sizeof(section));
    if (section.name >= stringSection.size) continue;
    const char* name = names + section.name;
    if (!std::memchr(name, '\0', stringSection.size - section.name) ||
        std::strcmp(name, crosspoint_plugin::METADATA_SECTION) != 0) {
      continue;
    }
    if (section.size != sizeof(descriptor) || !rangeWithin(section.offset, section.size, image.elfSize)) return false;
    std::memcpy(&descriptor, image.payload.get() + section.offset, sizeof(descriptor));
    return descriptor.title[0] != '\0' && std::memchr(descriptor.title, '\0', sizeof(descriptor.title)) &&
           std::memchr(descriptor.version, '\0', sizeof(descriptor.version));
  }
  return false;
}

enum class PluginKeyboardResultState : uint8_t { NONE, OPEN, COMPLETED, CANCELLED };

struct PluginKeyboardResult {
  PluginKeyboardResultState state = PluginKeyboardResultState::NONE;
  std::string text;
};

PluginKeyboardResult pluginKeyboardResult;

class PluginTextKeyboardActivity final : public KeyboardEntryActivity {
 public:
  PluginTextKeyboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* title,
                             const size_t maxLength, const bool showHeaderKeyboardToggle,
                             const bool enableSystemLanguageSwitch)
      : KeyboardEntryActivity(renderer, mappedInput, title, "", maxLength, InputType::Text, showHeaderKeyboardToggle,
                              enableSystemLanguageSwitch) {}

  void onExit() override {
    if (pluginKeyboardResult.state == PluginKeyboardResultState::OPEN) {
      pluginKeyboardResult.text.clear();
      pluginKeyboardResult.state = PluginKeyboardResultState::CANCELLED;
    }
    KeyboardEntryActivity::onExit();
  }

  bool preventAutoSleep() override { return true; }

 protected:
  void onComplete(std::string text) override {
    pluginKeyboardResult.text = std::move(text);
    pluginKeyboardResult.state = PluginKeyboardResultState::COMPLETED;
    finish();
  }

  void onCancel() override {
    pluginKeyboardResult.text.clear();
    pluginKeyboardResult.state = PluginKeyboardResultState::CANCELLED;
    finish();
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
  ValidatedModuleImage image;
  if (!readValidatedModuleImage(path, image, module.lastError)) return false;

  elf_set_symbol_resolver(resolvePluginSymbol);
  if (esp_elf_init(&module.elf) != 0) {
    module.lastError = Error::LOAD_FAILED;
    return false;
  }
  module.initialized = true;
  if (esp_elf_relocate(&module.elf, image.payload.get()) != 0) {
    LOG_ERR("PLUGIN", "ELF relocation failed: %s", path);
    unload(module);
    module.lastError = Error::LOAD_FAILED;
    return false;
  }
  module.loaded = true;
  LOG_INF("PLUGIN", "Loaded %u-byte module into PSRAM: %s", static_cast<unsigned>(image.elfSize), path);
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

Activity* PluginLoader::createChild(const char* moduleName, GfxRenderer& renderer, MappedInputManager& mappedInput) {
  if (!validModuleName(moduleName) || child_.rootActivity) return nullptr;
  std::array<char, 128> path{};
  const int length = std::snprintf(path.data(), path.size(), "%s/%s.so", crosspoint_plugin::ROOT_PATH, moduleName);
  if (length <= 0 || static_cast<size_t>(length) >= path.size()) return nullptr;
  auto activity = create(child_, path.data(), renderer, mappedInput);
  return activity.release();
}

bool PluginLoader::describeChild(const char* moduleName, crosspoint_plugin::PluginInfoV3* info) {
  if (!info || !validModuleName(moduleName) || child_.rootActivity || child_.loaded) return false;
  std::array<char, 128> path{};
  const int pathLength = std::snprintf(path.data(), path.size(), "%s/%s.so", crosspoint_plugin::ROOT_PATH, moduleName);
  if (pathLength <= 0 || static_cast<size_t>(pathLength) >= path.size()) return false;

  ValidatedModuleImage image;
  Error error = Error::NONE;
  if (!readValidatedModuleImage(path.data(), image, error)) return false;
  crosspoint_plugin::PluginDescriptorV3 descriptor{};
  if (!readPluginMetadata(image, descriptor)) {
    LOG_ERR("PLUGIN", "Metadata section is missing or invalid: %s", path.data());
    return false;
  }

  *info = {};
  std::snprintf(info->module, sizeof(info->module), "%s", moduleName);
  info->descriptor = descriptor;
  return true;
}

size_t PluginLoader::listChildren(crosspoint_plugin::PluginInfoV3* modules, const size_t capacity) {
  if (!modules || capacity == 0 || child_.rootActivity || child_.loaded) return 0;
  HalFile directory = Storage.open(crosspoint_plugin::ROOT_PATH);
  if (!directory || !directory.isDirectory()) return 0;

  size_t count = 0;
  for (HalFile file = directory.openNextFile(); file && count < capacity; file = directory.openNextFile()) {
    if (file.isDirectory()) continue;
    std::array<char, 96> filename{};
    const size_t length = file.getName(filename.data(), filename.size());
    if (length == 0 || length >= filename.size()) continue;
    filename[length] = '\0';
    if (length <= 3 || std::strcmp(filename.data() + length - 3, ".so") != 0) continue;
    filename[length - 3] = '\0';
    if (std::strcmp(filename.data(), "manager") == 0 || !validModuleName(filename.data())) continue;
    if (describeChild(filename.data(), &modules[count])) count++;
  }
  return count;
}

bool PluginLoader::beginInstall(const char* moduleName, const uint32_t bytes,
                                const uint8_t sha256[crosspoint_plugin::SHA256_BYTES]) {
  if (pluginInstall.active || child_.rootActivity || child_.loaded || !validModuleName(moduleName) ||
      std::strcmp(moduleName, "manager") == 0 || !sha256 || bytes <= crosspoint_plugin::TRAILER_BYTES ||
      bytes > crosspoint_plugin::MAX_ELF_BYTES + crosspoint_plugin::TRAILER_BYTES) {
    return false;
  }
  if (!Storage.ensureDirectoryExists(crosspoint_plugin::ROOT_PATH)) return false;

  const int pathLength = std::snprintf(pluginInstall.path.data(), pluginInstall.path.size(), "%s/%s.so",
                                       crosspoint_plugin::ROOT_PATH, moduleName);
  if (pathLength <= 0 || static_cast<size_t>(pathLength) >= pluginInstall.path.size() ||
      !Storage.openFileForWrite("PLUGIN", pluginInstall.path.data(), pluginInstall.file)) {
    clearPluginInstall();
    return false;
  }
  std::snprintf(pluginInstall.module.data(), pluginInstall.module.size(), "%s", moduleName);
  std::memcpy(pluginInstall.expectedSha.data(), sha256, pluginInstall.expectedSha.size());
  pluginInstall.expectedBytes = bytes;
  mbedtls_sha256_init(&pluginInstall.sha);
  pluginInstall.shaInitialized = true;
  if (mbedtls_sha256_starts(&pluginInstall.sha, 0) != 0) {
    clearPluginInstall();
    return false;
  }
  pluginInstall.active = true;
  return true;
}

bool PluginLoader::writeInstall(const uint32_t offset, const uint8_t* data, const size_t length) {
  if (!pluginInstall.active || !data || length == 0 || offset != pluginInstall.writtenBytes ||
      length > pluginInstall.expectedBytes - pluginInstall.writtenBytes ||
      pluginInstall.file.write(data, length) != length ||
      mbedtls_sha256_update(&pluginInstall.sha, data, length) != 0) {
    return false;
  }
  pluginInstall.writtenBytes += length;
  return true;
}

bool PluginLoader::finishInstall() {
  if (!pluginInstall.active || pluginInstall.writtenBytes != pluginInstall.expectedBytes) return false;

  std::array<uint8_t, crosspoint_plugin::SHA256_BYTES> actualSha{};
  const bool digestOk = mbedtls_sha256_finish(&pluginInstall.sha, actualSha.data()) == 0 &&
                        std::memcmp(actualSha.data(), pluginInstall.expectedSha.data(), actualSha.size()) == 0;
  mbedtls_sha256_free(&pluginInstall.sha);
  pluginInstall.shaInitialized = false;
  pluginInstall.file.flush();
  pluginInstall.file.close();
  pluginInstall.active = false;

  crosspoint_plugin::PluginInfoV3 info{};
  const bool validModule = digestOk && describeChild(pluginInstall.module.data(), &info);
  clearPluginInstall();
  return validModule;
}

void PluginLoader::abortInstall() { clearPluginInstall(); }

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

extern "C" uint8_t crosspoint_plugin_open_text_keyboard_v2(const char* title, const size_t maxLength,
                                                           const uint32_t flags, GfxRenderer* renderer,
                                                           MappedInputManager* mappedInput) {
  if (!renderer || !mappedInput || !title || maxLength == 0 ||
      pluginKeyboardResult.state == PluginKeyboardResultState::OPEN) {
    return false;
  }
  pluginKeyboardResult.text.clear();
  pluginKeyboardResult.state = PluginKeyboardResultState::OPEN;
  auto keyboard = makeUniqueNoThrow<PluginTextKeyboardActivity>(
      *renderer, *mappedInput, title, maxLength, (flags & crosspoint_plugin::KEYBOARD_FLAG_HEADER_TOGGLE) != 0,
      (flags & crosspoint_plugin::KEYBOARD_FLAG_SYSTEM_LANGUAGE) != 0);
  if (!keyboard) {
    pluginKeyboardResult.state = PluginKeyboardResultState::NONE;
    return false;
  }
  activityManager.pushActivity(std::move(keyboard));
  return true;
}

extern "C" uint8_t crosspoint_plugin_take_text_keyboard_result_v2(char* text, const size_t capacity, size_t* length,
                                                                  uint8_t* cancelled) {
  if (!length || !cancelled ||
      (pluginKeyboardResult.state != PluginKeyboardResultState::COMPLETED &&
       pluginKeyboardResult.state != PluginKeyboardResultState::CANCELLED)) {
    return false;
  }

  const bool wasCancelled = pluginKeyboardResult.state == PluginKeyboardResultState::CANCELLED;
  const size_t resultLength = wasCancelled ? 0 : pluginKeyboardResult.text.size();
  if (!wasCancelled && (!text || capacity <= resultLength)) return false;

  if (!wasCancelled) {
    std::memcpy(text, pluginKeyboardResult.text.data(), resultLength);
    text[resultLength] = '\0';
  } else if (text && capacity != 0) {
    text[0] = '\0';
  }
  *length = resultLength;
  *cancelled = wasCancelled;
  pluginKeyboardResult.text.clear();
  pluginKeyboardResult.state = PluginKeyboardResultState::NONE;
  return true;
}

extern "C" size_t crosspoint_plugin_list_v3(crosspoint_plugin::PluginInfoV3* modules, const size_t capacity) {
  return PLUGIN_LOADER.listChildren(modules, std::min(capacity, crosspoint_plugin::MAX_LISTED_MODULES));
}

extern "C" uint8_t crosspoint_plugin_install_begin_v3(const char* module, const uint32_t bytes,
                                                      const uint8_t sha256[crosspoint_plugin::SHA256_BYTES]) {
  return PLUGIN_LOADER.beginInstall(module, bytes, sha256);
}

extern "C" uint8_t crosspoint_plugin_install_write_v3(const uint32_t offset, const uint8_t* data, const size_t length) {
  return PLUGIN_LOADER.writeInstall(offset, data, length);
}

extern "C" uint8_t crosspoint_plugin_install_finish_v3() { return PLUGIN_LOADER.finishInstall(); }

extern "C" void crosspoint_plugin_install_abort_v3() { PLUGIN_LOADER.abortInstall(); }

extern "C" uint8_t crosspoint_plugin_ble_start_v4() { return plugin_ble::sharedPluginBleTransport().start(); }

extern "C" void crosspoint_plugin_ble_stop_v4() { plugin_ble::sharedPluginBleTransport().stop(); }

extern "C" uint8_t crosspoint_plugin_ble_poll_v4(uint8_t* packet, const size_t capacity, size_t* length) {
  if (!packet || !length) return false;
  plugin_ble::PluginBleTransport::IncomingPacket incoming{};
  if (!plugin_ble::sharedPluginBleTransport().poll(incoming) || capacity < incoming.length) return false;
  std::memcpy(packet, incoming.bytes.data(), incoming.length);
  *length = incoming.length;
  return true;
}

extern "C" uint8_t crosspoint_plugin_ble_send_v4(const uint8_t* packet, const size_t length) {
  return plugin_ble::sharedPluginBleTransport().send(packet, length);
}

extern "C" uint8_t crosspoint_plugin_ble_ready_v4() { return plugin_ble::sharedPluginBleTransport().readyToSend(); }

extern "C" size_t crosspoint_plugin_ble_max_packet_bytes_v4() {
  return plugin_ble::sharedPluginBleTransport().maxPacketBytes();
}

extern "C" void crosspoint_plugin_ble_set_transfer_active_v4(const uint8_t active) {
  plugin_ble::sharedPluginBleTransport().setTransferActive(active != 0);
}

extern "C" crosspoint_plugin::PluginBleStatusV4 crosspoint_plugin_ble_status_v4() {
  return static_cast<crosspoint_plugin::PluginBleStatusV4>(plugin_ble::sharedPluginBleTransport().status());
}

extern "C" uint32_t crosspoint_plugin_ble_status_revision_v4() {
  return plugin_ble::sharedPluginBleTransport().statusRevision();
}

extern "C" uint32_t crosspoint_plugin_ble_dropped_packets_v4() {
  return plugin_ble::sharedPluginBleTransport().droppedPackets();
}

extern "C" uint32_t crosspoint_plugin_ble_pairing_passkey_v4() {
  return plugin_ble::sharedPluginBleTransport().pairingPasskey();
}

#endif
