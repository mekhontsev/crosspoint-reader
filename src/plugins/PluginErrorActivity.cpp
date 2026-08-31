#include "PluginErrorActivity.h"

#if defined(ENABLE_PLUGINS) && ENABLE_PLUGINS

#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

PluginErrorActivity::PluginErrorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const char* message)
    : Activity("PluginError", renderer, mappedInput), message_(message ? message : "") {}

void PluginErrorActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onGoHome(HomeMenuItem::PLUGINS);
  }
}

bool PluginErrorActivity::handleHomeGesture() {
  onGoHome(HomeMenuItem::PLUGINS);
  return true;
}

void PluginErrorActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int bodyTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bodyHeight = pageHeight - bodyTop - metrics.buttonHintsHeight;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PLUGINS));
  UITheme::drawCenteredWrappedText(
      renderer,
      Rect{metrics.contentSidePadding, bodyTop, pageWidth - 2 * metrics.contentSidePadding, bodyHeight},
      UI_10_FONT_ID, message_.c_str(), 5);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif
