#include "BleTerminalActivity.h"

#if defined(ENABLE_BLE_TERMINAL) && ENABLE_BLE_TERMINAL

#include <Arduino.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_memory_utils.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/HeaderKeyboardButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr size_t DISPLAY_LINE_BYTES = 256;
constexpr size_t HEADER_CONTROL_COUNT = 3;
constexpr int HEADER_TITLE_GAP = 20;
constexpr unsigned long JUMP_TO_TAIL_HOLD_MS = 700;
constexpr std::array<int, 9> TERMINAL_FONT_IDS = {
    TERMINAL_MONO_8_FONT_ID,  TERMINAL_MONO_10_FONT_ID, TERMINAL_MONO_12_FONT_ID,
    TERMINAL_MONO_14_FONT_ID, TERMINAL_MONO_16_FONT_ID, TERMINAL_MONO_18_FONT_ID,
    TERMINAL_MONO_20_FONT_ID, TERMINAL_MONO_22_FONT_ID, TERMINAL_MONO_24_FONT_ID,
};

struct VisualLine {
  size_t contentEnd = 0;
  size_t nextStart = 0;
};

bool isUtf8Continuation(const char value) {
  return (static_cast<uint8_t>(value) & 0xC0U) == 0x80U;
}

VisualLine prepareVisualLine(const GfxRenderer& renderer, const int fontId, const char* source, const size_t textLength,
                             const size_t start, const int maxWidth,
                             std::array<char, DISPLAY_LINE_BYTES>& line) {
  size_t logicalEnd = start;
  while (logicalEnd < textLength && source[logicalEnd] != '\n') ++logicalEnd;

  const size_t available = logicalEnd - start;
  size_t length = std::min(available, line.size() - 1);
  while (length > 0 && start + length < logicalEnd && isUtf8Continuation(source[start + length])) --length;

  std::memcpy(line.data(), source + start, length);
  for (size_t i = 0; i < length; ++i) {
    if (line[i] == '\t') line[i] = ' ';
  }
  line[length] = '\0';

  while (length > 0 && renderer.getTextWidth(fontId, line.data()) > maxWidth) {
    size_t newLength = length - 1;
    while (newLength > 0 && isUtf8Continuation(line[newLength])) --newLength;
    length = newLength;
    line[length] = '\0';
  }

  // Always consume at least one complete code point, even if a single glyph is
  // wider than the viewport. This keeps pagination progressing on malformed
  // font metrics without splitting UTF-8.
  if (length == 0 && available > 0) {
    length = 1;
    while (start + length < logicalEnd && isUtf8Continuation(source[start + length])) ++length;
    std::memcpy(line.data(), source + start, length);
    if (line[0] == '\t') line[0] = ' ';
    line[length] = '\0';
  }

  VisualLine result;
  result.contentEnd = start + length;
  if (length < available) {
    result.nextStart = result.contentEnd;
  } else if (logicalEnd < textLength) {
    result.nextStart = logicalEnd + 1;
  } else {
    result.nextStart = textLength + 1;
  }
  return result;
}

size_t countVisualLines(const GfxRenderer& renderer, const int fontId, const char* text, const size_t textLength,
                        const int maxWidth, std::array<char, DISPLAY_LINE_BYTES>& line) {
  size_t count = 0;
  size_t cursor = 0;
  while (cursor <= textLength) {
    const VisualLine visualLine = prepareVisualLine(renderer, fontId, text, textLength, cursor, maxWidth, line);
    ++count;
    cursor = visualLine.nextStart;
  }
  return count;
}

size_t visualLineStart(const GfxRenderer& renderer, const int fontId, const char* text, const size_t textLength,
                       const int maxWidth, const size_t targetIndex,
                       std::array<char, DISPLAY_LINE_BYTES>& line) {
  size_t index = 0;
  size_t cursor = 0;
  while (cursor <= textLength && index < targetIndex) {
    cursor = prepareVisualLine(renderer, fontId, text, textLength, cursor, maxWidth, line).nextStart;
    ++index;
  }
  return std::min(cursor, textLength);
}

size_t visualLineIndexAtOrBefore(const GfxRenderer& renderer, const int fontId, const char* text,
                                 const size_t textLength, const int maxWidth, const size_t byteOffset,
                                 std::array<char, DISPLAY_LINE_BYTES>& line) {
  size_t index = 0;
  size_t cursor = 0;
  while (cursor <= textLength) {
    const size_t next = prepareVisualLine(renderer, fontId, text, textLength, cursor, maxWidth, line).nextStart;
    if (next > byteOffset || next > textLength) return index;
    cursor = next;
    ++index;
  }
  return index;
}

}  // namespace

BleTerminalActivity::BleTerminalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BleTerminal", renderer, mappedInput),
      receiver_(transcript_.data(), transcript_.size()),
      transcriptMutex_(xSemaphoreCreateMutexStatic(&transcriptMutexStorage_)) {}

void BleTerminalActivity::onEnter() {
  Activity::onEnter();

  transport_.start();
  observedStatusRevision_ = transport_.statusRevision();
  screenDirty_ = false;
  needsReset_ = false;
  commandSendFailed_ = false;
  followTail_ = true;
  viewportStart_ = 0;
  dirtySince_ = 0;
  lastPacketAt_ = 0;
  lastTransferActivityAt_ = 0;
  lastDisplayRequestAt_ = millis();
  LOG_INF("BLE_TERM", "Transcript buffer: %u bytes in %s; free heap=%u, PSRAM=%u",
          static_cast<unsigned>(ble_terminal::MAX_TRANSCRIPT_BYTES),
          esp_ptr_external_ram(transcript_.data()) ? "PSRAM" : "internal RAM", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getFreePsram()));
  requestUpdate();
}

void BleTerminalActivity::onExit() {
  transport_.stop();
  if (transcriptMutex_ && xSemaphoreTake(transcriptMutex_, portMAX_DELAY) == pdTRUE) {
    receiver_.clear();
    needsReset_ = false;
    xSemaphoreGive(transcriptMutex_);
  }
  Activity::onExit();
}

void BleTerminalActivity::markScreenDirty(const unsigned long now) {
  if (!screenDirty_) dirtySince_ = now;
  screenDirty_ = true;
}

void BleTerminalActivity::openCommandKeyboard() {
  const size_t maxLength = transport_.maxCommandBytes();
  if (maxLength == 0) return;

  commandSendFailed_ = false;
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TERMINAL_COMMAND), "",
                                                            maxLength, InputType::Text,
                                                            /*showHeaderKeyboardToggle=*/true,
                                                            /*enableSystemLanguageSwitch=*/true);
  if (!keyboard) {
    LOG_ERR("BLE_TERM", "Unable to allocate command keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* keyboardResult = std::get_if<KeyboardResult>(&result.data);
    if (!keyboardResult) return;

    // OK/Enter submits exactly one line. An empty line reuses the existing
    // allow-listed Enter action; non-empty text is a COMMAND whose receiver
    // appends the terminal Enter key after injecting the payload.
    const bool sent = keyboardResult->text.empty()
                          ? transport_.sendAction(ble_terminal::Action::SUBMIT_INPUT)
                          : transport_.sendCommand(keyboardResult->text);
    if (!sent) {
      commandSendFailed_ = true;
      markScreenDirty(millis());
    }
  });
}

int BleTerminalActivity::terminalFontId() const { return TERMINAL_FONT_IDS[fontSizeIndex_]; }

void BleTerminalActivity::changeFontSize(const int direction) {
  const int nextIndex = std::clamp(static_cast<int>(fontSizeIndex_) + direction, 0,
                                   static_cast<int>(TERMINAL_FONT_IDS.size()) - 1);
  if (nextIndex == fontSizeIndex_) return;
  fontSizeIndex_ = static_cast<uint8_t>(nextIndex);
  requestUpdate();
}

void BleTerminalActivity::scrollPage(const int direction) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bodyTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bodyHeight = renderer.getScreenHeight() - bodyTop - metrics.buttonHintsHeight;
  const int bodyWidth = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(terminalFontId());
  const int maxLines = lineHeight > 0 ? bodyHeight / lineHeight : 0;
  if (maxLines <= 0 || !transcriptMutex_ || xSemaphoreTake(transcriptMutex_, portMAX_DELAY) != pdTRUE) return;

  bool changed = false;
  const size_t textLength = receiver_.currentLength();
  if (textLength > 0) {
    const char* text = receiver_.currentText();
    const size_t totalLines =
        countVisualLines(renderer, terminalFontId(), text, textLength, bodyWidth, displayLine_);
    const size_t visibleLines = static_cast<size_t>(maxLines);
    const size_t tailIndex = totalLines > visibleLines ? totalLines - visibleLines : 0;
    const size_t currentIndex =
        followTail_ ? tailIndex
                    : std::min(visualLineIndexAtOrBefore(renderer, terminalFontId(), text, textLength, bodyWidth,
                                                         viewportStart_, displayLine_),
                               tailIndex);
    const size_t pageLines = std::max<size_t>(1, visibleLines - 1);

    size_t targetIndex = currentIndex;
    if (direction < 0) {
      targetIndex = currentIndex > pageLines ? currentIndex - pageLines : 0;
    } else if (direction > 0) {
      targetIndex = std::min(tailIndex, currentIndex + pageLines);
    }

    const bool nextFollowTail = targetIndex == tailIndex;
    if (targetIndex != currentIndex || nextFollowTail != followTail_) {
      followTail_ = nextFollowTail;
      viewportStart_ = visualLineStart(renderer, terminalFontId(), text, textLength, bodyWidth, targetIndex,
                                       displayLine_);
      changed = true;
    }
  }
  xSemaphoreGive(transcriptMutex_);

  if (changed) requestUpdate();
}

void BleTerminalActivity::jumpToTail() {
  if (!transcriptMutex_ || xSemaphoreTake(transcriptMutex_, portMAX_DELAY) != pdTRUE) return;
  const bool changed = !followTail_;
  followTail_ = true;
  viewportStart_ = 0;
  xSemaphoreGive(transcriptMutex_);

  if (changed) requestUpdate();
}

void BleTerminalActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::TERMINAL);
    return;
  }

  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const char* title = tr(STR_X4_TERMINAL);
    const Rect keyboardButton =
        header_keyboard_button::layout(renderer, metrics, title, 0, HEADER_CONTROL_COUNT, HEADER_TITLE_GAP);
    if (mappedInput.wasTapInRect(keyboardButton.x, keyboardButton.y, keyboardButton.width, keyboardButton.height)) {
      if (transport_.readyToSend()) openCommandKeyboard();
      return;
    }

    const Rect decreaseButton =
        header_keyboard_button::layout(renderer, metrics, title, 1, HEADER_CONTROL_COUNT, HEADER_TITLE_GAP);
    if (mappedInput.wasTapInRect(decreaseButton.x, decreaseButton.y, decreaseButton.width, decreaseButton.height)) {
      changeFontSize(-1);
      return;
    }

    const Rect increaseButton =
        header_keyboard_button::layout(renderer, metrics, title, 2, HEADER_CONTROL_COUNT, HEADER_TITLE_GAP);
    if (mappedInput.wasTapInRect(increaseButton.x, increaseButton.y, increaseButton.width, increaseButton.height)) {
      changeFontSize(1);
      return;
    }
  }

  // Keep short-range history navigation local so it works immediately and
  // causes no BLE traffic or phone wake-up. Holding Page Down jumps directly
  // to the live tail; wasLongPressed() suppresses the matching release so it
  // cannot also advance one page.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Down, JUMP_TO_TAIL_HOLD_MS)) {
    jumpToTail();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    scrollPage(-1);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    scrollPage(1);
  }

  const unsigned long now = millis();
  ble_terminal::BleTerminalTransport::IncomingPacket packet;
  while (transport_.poll(packet)) {
    if (!transcriptMutex_ || xSemaphoreTake(transcriptMutex_, portMAX_DELAY) != pdTRUE) continue;

    const ble_terminal::AcceptResult result = receiver_.accept(packet.bytes.data(), packet.length);
    const bool streamChanged =
        result == ble_terminal::AcceptResult::STREAM_RESET || result == ble_terminal::AcceptResult::TEXT_APPENDED;
    const bool validStreamPacket = streamChanged || result == ble_terminal::AcceptResult::DUPLICATE_IGNORED;
    const bool protocolNeedsReset = result == ble_terminal::AcceptResult::NEEDS_RESET ||
                                    result == ble_terminal::AcceptResult::OUT_OF_ORDER ||
                                    result == ble_terminal::AcceptResult::TOO_LARGE ||
                                    result == ble_terminal::AcceptResult::INVALID_TEXT ||
                                    result == ble_terminal::AcceptResult::UNSUPPORTED_VERSION;

    const size_t discardedBytes = receiver_.lastDiscardedBytes();
    if (result == ble_terminal::AcceptResult::STREAM_RESET) {
      needsReset_ = false;
      followTail_ = true;
      viewportStart_ = 0;
    } else if (!followTail_ && discardedBytes > 0) {
      viewportStart_ = discardedBytes < viewportStart_ ? viewportStart_ - discardedBytes : 0;
    }
    if (protocolNeedsReset) needsReset_ = true;
    xSemaphoreGive(transcriptMutex_);

    if (validStreamPacket || protocolNeedsReset) {
      lastPacketAt_ = now;
      lastTransferActivityAt_ = now;
      transport_.setTransferActive(true);
    }
    if ((streamChanged && (followTail_ || discardedBytes > 0)) || protocolNeedsReset) markScreenDirty(now);
  }

  const uint32_t statusRevision = transport_.statusRevision();
  if (statusRevision != observedStatusRevision_) {
    observedStatusRevision_ = statusRevision;
    if (transport_.status() == ble_terminal::BleTerminalTransport::Status::CONNECTED) {
      lastTransferActivityAt_ = now;
      transport_.setTransferActive(true);
    }
    markScreenDirty(now);
  }

  if (lastTransferActivityAt_ != 0 && now - lastTransferActivityAt_ >= TRANSFER_IDLE_MS) {
    lastTransferActivityAt_ = 0;
    transport_.setTransferActive(false);
  }

  if (!screenDirty_) return;

  const bool refreshIntervalElapsed = now - lastDisplayRequestAt_ >= MIN_DISPLAY_REFRESH_MS;
  const bool streamIsQuiet = lastPacketAt_ == 0 || now - lastPacketAt_ >= STREAM_QUIET_MS;
  const bool maximumLatencyElapsed = now - dirtySince_ >= MAX_DIRTY_LATENCY_MS;
  if (refreshIntervalElapsed && (streamIsQuiet || maximumLatencyElapsed)) {
    screenDirty_ = false;
    lastDisplayRequestAt_ = now;
    requestUpdate();
  }
}

bool BleTerminalActivity::preventAutoSleep() {
  return lastPacketAt_ != 0 && millis() - lastPacketAt_ < DATA_KEEP_AWAKE_MS;
}

void BleTerminalActivity::formatStatusText(char* buffer, const size_t bufferSize) const {
  if (commandSendFailed_) {
    snprintf(buffer, bufferSize, "%s", tr(STR_TERMINAL_COMMAND_SEND_FAILED));
    return;
  }
  if (needsReset_) {
    snprintf(buffer, bufferSize, "%s", tr(STR_TERMINAL_BLE_RESYNC));
    return;
  }

  switch (transport_.status()) {
    case ble_terminal::BleTerminalTransport::Status::ADVERTISING:
      snprintf(buffer, bufferSize, "%s", tr(STR_TERMINAL_BLE_WAITING));
      return;
    case ble_terminal::BleTerminalTransport::Status::PAIRING: {
      const uint32_t passkey = transport_.pairingPasskey();
      if (passkey == 0) {
        snprintf(buffer, bufferSize, "%s", tr(STR_TERMINAL_BLE_SECURING));
      } else {
        snprintf(buffer, bufferSize, tr(STR_TERMINAL_BLE_PAIRING), static_cast<unsigned long>(passkey));
      }
      return;
    }
    case ble_terminal::BleTerminalTransport::Status::CONNECTED:
      snprintf(buffer, bufferSize, "%s", tr(STR_TERMINAL_BLE_CONNECTED));
      return;
    case ble_terminal::BleTerminalTransport::Status::ERROR:
      snprintf(buffer, bufferSize, "%s", tr(STR_TERMINAL_BLE_ERROR));
      return;
    case ble_terminal::BleTerminalTransport::Status::STARTING:
    case ble_terminal::BleTerminalTransport::Status::STOPPED:
    default:
      snprintf(buffer, bufferSize, "%s", tr(STR_TERMINAL_BLE_STARTING));
      return;
  }
}

void BleTerminalActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_X4_TERMINAL));
  const char* title = tr(STR_X4_TERMINAL);
  header_keyboard_button::draw(
      renderer, header_keyboard_button::layout(renderer, metrics, title, 0, HEADER_CONTROL_COUNT, HEADER_TITLE_GAP));
  header_keyboard_button::draw(
      renderer, header_keyboard_button::layout(renderer, metrics, title, 1, HEADER_CONTROL_COUNT, HEADER_TITLE_GAP),
      header_keyboard_button::Glyph::FONT_DECREASE);
  header_keyboard_button::draw(
      renderer, header_keyboard_button::layout(renderer, metrics, title, 2, HEADER_CONTROL_COUNT, HEADER_TITLE_GAP),
      header_keyboard_button::Glyph::FONT_INCREASE);

  const int bodyTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bodyHeight = pageHeight - bodyTop - metrics.buttonHintsHeight;
  const int bodyWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int fontId = terminalFontId();
  const int lineHeight = renderer.getLineHeight(fontId);
  const int maxLines = lineHeight > 0 ? bodyHeight / lineHeight : 0;

  if (transcriptMutex_ && xSemaphoreTake(transcriptMutex_, portMAX_DELAY) == pdTRUE) {
    const size_t textLength = receiver_.currentLength();
    const auto status = transport_.status();
    const bool securityStatusNeedsScreen = status == ble_terminal::BleTerminalTransport::Status::PAIRING ||
                                           status == ble_terminal::BleTerminalTransport::Status::ERROR;
    if (commandSendFailed_ || needsReset_ || textLength == 0 || maxLines <= 0 || securityStatusNeedsScreen) {
      std::array<char, 192> statusMessage{};
      formatStatusText(statusMessage.data(), statusMessage.size());
      const uint32_t pairingPasskey = transport_.pairingPasskey();
      if (status == ble_terminal::BleTerminalTransport::Status::PAIRING && pairingPasskey != 0) {
        // The general wrapping helper treats an explicit newline as a
        // zero-width glyph when the complete string fits. Draw the code and
        // instruction as separate blocks so they can never be concatenated.
        const int codeLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
        const int hintLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
        const int hintHeight = hintLineHeight * 2;
        const int blockHeight = codeLineHeight + metrics.verticalSpacing + hintHeight;
        const int codeY = bodyTop + (bodyHeight - blockHeight) / 2;
        UITheme::drawCenteredText(renderer, Rect{metrics.contentSidePadding, bodyTop, bodyWidth, bodyHeight},
                                  UI_12_FONT_ID, codeY, statusMessage.data(), true, EpdFontFamily::BOLD);
        UITheme::drawCenteredWrappedText(
            renderer,
            Rect{metrics.contentSidePadding, codeY + codeLineHeight + metrics.verticalSpacing, bodyWidth, hintHeight},
            UI_10_FONT_ID, tr(STR_TERMINAL_BLE_PAIRING_HINT), 2);
      } else {
        UITheme::drawCenteredWrappedText(
            renderer, Rect{metrics.contentSidePadding, bodyTop, bodyWidth, bodyHeight}, UI_10_FONT_ID,
            statusMessage.data(), 5);
      }
    } else {
      const char* text = receiver_.currentText();
      const size_t totalLines = countVisualLines(renderer, fontId, text, textLength, bodyWidth, displayLine_);
      const size_t visibleLines = static_cast<size_t>(maxLines);
      const size_t tailIndex = totalLines > visibleLines ? totalLines - visibleLines : 0;
      size_t firstIndex = tailIndex;
      if (!followTail_) {
        firstIndex = std::min(visualLineIndexAtOrBefore(renderer, fontId, text, textLength, bodyWidth, viewportStart_,
                                                       displayLine_),
                              tailIndex);
        viewportStart_ = visualLineStart(renderer, fontId, text, textLength, bodyWidth, firstIndex, displayLine_);
      }

      size_t cursor = visualLineStart(renderer, fontId, text, textLength, bodyWidth, firstIndex, displayLine_);
      int y = bodyTop;
      for (int displayedLine = 0; displayedLine < maxLines && cursor <= textLength; ++displayedLine) {
        const VisualLine visualLine =
            prepareVisualLine(renderer, fontId, text, textLength, cursor, bodyWidth, displayLine_);
        if (displayLine_[0] != '\0') renderer.drawText(fontId, metrics.contentSidePadding, y, displayLine_.data());
        y += lineHeight;
        cursor = visualLine.nextStart;
      }
    }
    xSemaphoreGive(transcriptMutex_);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif
