#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstddef>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace header_keyboard_button {

enum class Glyph { KEYBOARD, FONT_DECREASE, FONT_INCREASE };

inline Rect layout(const GfxRenderer& renderer, const ThemeMetrics& metrics, const char* title,
                   const size_t slot = 0, const size_t slotCount = 1, const int titleGap = 8) {
  constexpr int width = 48;
  constexpr int height = 30;
  constexpr int buttonGap = 6;
  constexpr int titleBottomGap = 8;  // FreeInkUI ThemeTokens::spaceMd

  const size_t normalizedSlotCount = std::max<size_t>(1, slotCount);
  const size_t normalizedSlot = std::min(slot, normalizedSlotCount - 1);
  const int screenWidth = renderer.getScreenWidth();
  const int titleWidth = title ? renderer.getTextWidth(UI_12_FONT_ID, title) : 0;
  int titleX = metrics.headerSidePadding;
  if (metrics.headerTitleAlign == 1) {
    titleX = (screenWidth - titleWidth) / 2;
  } else if (metrics.headerTitleAlign == 2) {
    titleX = screenWidth - metrics.headerSidePadding - titleWidth;
  }

  // Match BaseTheme::drawHeader's title line: Lyra puts the title in the
  // lower sub-band, while shared-line headers vertically center it.
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int titleTop = metrics.headerBatteryDetached
                           ? metrics.topPadding + metrics.headerHeight - metrics.headerUnderlineSize -
                                 titleBottomGap - titleLineHeight
                           : metrics.topPadding + (metrics.headerHeight - titleLineHeight) / 2;
  const int y = titleTop + (titleLineHeight - height) / 2;
  const int groupWidth = static_cast<int>(normalizedSlotCount) * width +
                         static_cast<int>(normalizedSlotCount - 1) * buttonGap;
  const int groupRightLimit = screenWidth - metrics.headerSidePadding - groupWidth;
  const int groupX = std::min(titleX + titleWidth + titleGap, groupRightLimit);
  const int x = groupX + static_cast<int>(normalizedSlot) * (width + buttonGap);
  return Rect{x, y, width, height};
}

inline void draw(const GfxRenderer& renderer, const Rect& button, const Glyph glyph = Glyph::KEYBOARD) {
  renderer.drawRect(button.x, button.y, button.width, button.height);

  if (glyph == Glyph::FONT_DECREASE || glyph == Glyph::FONT_INCREASE) {
    constexpr int glyphWidth = 20;
    const int glyphX = button.x + (button.width - glyphWidth) / 2;
    const int glyphY = button.y + button.height / 2;
    renderer.drawLine(glyphX, glyphY, glyphX + glyphWidth, glyphY);
    if (glyph == Glyph::FONT_INCREASE) {
      renderer.drawLine(button.x + button.width / 2, glyphY - glyphWidth / 2, button.x + button.width / 2,
                        glyphY + glyphWidth / 2);
    }
    return;
  }

  // Small keyboard pictogram built from drawing primitives so it is crisp on
  // every monochrome theme and needs no font glyph or bitmap allocation.
  constexpr int glyphWidth = 30;
  constexpr int glyphHeight = 16;
  const int glyphX = button.x + (button.width - glyphWidth) / 2;
  const int glyphY = button.y + (button.height - glyphHeight) / 2;
  renderer.drawRect(glyphX, glyphY, glyphWidth, glyphHeight);
  for (int row = 0; row < 2; ++row) {
    for (int col = 0; col < 5; ++col) {
      renderer.fillRect(glyphX + 4 + col * 5, glyphY + 4 + row * 4, 2, 2);
    }
  }
  renderer.drawLine(glyphX + 9, glyphY + 12, glyphX + glyphWidth - 9, glyphY + 12);
}

}  // namespace header_keyboard_button
