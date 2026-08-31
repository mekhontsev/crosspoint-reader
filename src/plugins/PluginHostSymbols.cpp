#include "PluginHostSymbols.h"

#if defined(ENABLE_PLUGINS) && ENABLE_PLUGINS

#include <cstring>

#define PLUGIN_HOST_SYMBOL(id, symbolName) extern "C" char pluginHostSymbol##id asm(symbolName);
#include "PluginHostSymbols.inc"
#undef PLUGIN_HOST_SYMBOL

namespace {

struct HostSymbol {
  const char* name;
  uintptr_t address;
};

const HostSymbol HOST_SYMBOLS[] = {
#define PLUGIN_HOST_SYMBOL(id, symbolName) {symbolName, reinterpret_cast<uintptr_t>(&pluginHostSymbol##id)},
#include "PluginHostSymbols.inc"
#undef PLUGIN_HOST_SYMBOL
};

}  // namespace

uintptr_t resolvePluginHostSymbol(const char* name) {
  if (!name) return 0;
  for (const auto& symbol : HOST_SYMBOLS) {
    if (std::strcmp(name, symbol.name) == 0) return symbol.address;
  }
  return 0;
}

#endif
