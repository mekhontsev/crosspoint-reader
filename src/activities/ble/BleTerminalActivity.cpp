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
constexpr unsigned long CONTROL_RETRY_MS = 100;
constexpr unsigned long CONTROL_SETTLE_MS = 250;
constexpr unsigned long LONG_PRESS_MS = 700;
constexpr std::array<int, 9> TERMINAL_FONT_IDS = {
    TERMINAL_MONO_8_FONT_ID,  TERMINAL_MONO_10_FONT_ID, TERMINAL_MONO_12_FONT_ID,
    TERMINAL_MONO_14_FONT_ID, TERMINAL_MONO_16_FONT_ID, TERMINAL_MONO_18_FONT_ID,
    TERMINAL_MONO_20_FONT_ID, TERMINAL_MONO_22_FONT_ID, TERMINAL_MONO_24_FONT_ID,
};

struct VisualLine {
  size_t contentEnd = 0;
  size_t nextStart = 0;
};

bool isUtf8Continuation(const char value) { return (static_cast<uint8_t>(value) & 0xC0U) == 0x80U; }

bool frameIdBefore(const uint32_t first, const uint32_t second) { return static_cast<int32_t>(first - second) < 0; }

VisualLine prepareVisualLine(const GfxRenderer& renderer, const int fontId, const char* source, const size_t textLength,
                             const size_t start, const int maxWidth, std::array<char, DISPLAY_LINE_BYTES>& line) {
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
    cursor = prepareVisualLine(renderer, fontId, text, textLength, cursor, maxWidth, line).nextStart;
    ++count;
  }
  return count;
}

size_t visualLineStart(const GfxRenderer& renderer, const int fontId, const char* text, const size_t textLength,
                       const int maxWidth, const size_t targetIndex, std::array<char, DISPLAY_LINE_BYTES>& line) {
  size_t index = 0;
  size_t cursor = 0;
  while (cursor <= textLength && index < targetIndex) {
    cursor = prepareVisualLine(renderer, fontId, text, textLength, cursor, maxWidth, line).nextStart;
    ++index;
  }
  return std::min(cursor, textLength);
}

}  // namespace

BleTerminalActivity::BleTerminalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BleTerminal", renderer, mappedInput),
      transport_(ble_terminal::sharedTransport()),
      receiver_(stagingFrame_.data(), stagingFrame_.size()),
      frameMutex_(xSemaphoreCreateMutexStatic(&frameMutexStorage_)) {}

void BleTerminalActivity::onEnter() {
  Activity::onEnter();

  transport_.start();
  observedStatusRevision_ = transport_.statusRevision();
  pendingStatusFrameId_ = 0;
  pendingRequestAnchor_ = 0;
  readyAfterRenderFrameId_ = 0;
  lastPacketAt_ = 0;
  lastTransferActivityAt_ = 0;
  nextControlAttemptAt_ = 0;
  selectedSlot_ = -1;
  hasPendingStatus_ = false;
  hasPendingRequest_ = false;
  initialFrameRequested_ = false;
  needsReset_ = false;
  commandSendFailed_ = false;
  followLatest_ = true;
  cleanRequestedFrame_ = false;
  cleanRefreshPending_ = false;
  receiver_.clear();
  clearFrameCache();
  LOG_INF("BLE_TERM", "Frame cache: %u slots x %u bytes plus staging in %s; free heap=%u, PSRAM=%u",
          static_cast<unsigned>(FRAME_CACHE_SLOTS), static_cast<unsigned>(ble_terminal::MAX_FRAME_BYTES),
          esp_ptr_external_ram(frames_.data()) ? "PSRAM" : "internal RAM", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getFreePsram()));
  requestUpdate();
}

void BleTerminalActivity::onExit() {
  LOG_INF("BLE_TERM", "Terminal activity exit: stopping transport");
  transport_.stop();
  if (frameMutex_ && xSemaphoreTake(frameMutex_, portMAX_DELAY) == pdTRUE) {
    receiver_.clear();
    clearFrameCache();
    xSemaphoreGive(frameMutex_);
  }
  LOG_INF("BLE_TERM", "Terminal activity exit: transport stopped");
  Activity::onExit();
}

void BleTerminalActivity::clearFrameCache() {
  for (auto& frame : frames_) {
    frame.id = 0;
    frame.length = 0;
    frame.occupied = false;
    frame.latest = false;
    frame.text[0] = '\0';
  }
  selectedSlot_ = -1;
  followLatest_ = true;
}

int BleTerminalActivity::findFrameSlot(const uint32_t frameId) const {
  for (size_t index = 0; index < frames_.size(); ++index) {
    if (frames_[index].occupied && frames_[index].id == frameId) return static_cast<int>(index);
  }
  return -1;
}

int BleTerminalActivity::chooseFrameSlot(const uint32_t frameId) const {
  for (size_t index = 0; index < frames_.size(); ++index) {
    if (!frames_[index].occupied) return static_cast<int>(index);
  }

  int candidate = -1;
  for (size_t index = 0; index < frames_.size(); ++index) {
    if (static_cast<int>(index) == selectedSlot_) continue;
    if (candidate < 0) {
      candidate = static_cast<int>(index);
      continue;
    }

    const bool incomingIsNewer = selectedSlot_ < 0 || frameIdBefore(frames_[selectedSlot_].id, frameId);
    if ((incomingIsNewer && frameIdBefore(frames_[index].id, frames_[candidate].id)) ||
        (!incomingIsNewer && frameIdBefore(frames_[candidate].id, frames_[index].id))) {
      candidate = static_cast<int>(index);
    }
  }
  return candidate >= 0 ? candidate : 0;
}

uint32_t BleTerminalActivity::selectedFrameId() const {
  return selectedSlot_ >= 0 && frames_[selectedSlot_].occupied ? frames_[selectedSlot_].id : 0;
}

bool BleTerminalActivity::cacheCommittedFrame() {
  const uint8_t flags = receiver_.frameFlags();
  if ((flags & ble_terminal::FRAME_FLAG_RESET_CACHE) != 0) clearFrameCache();

  int slot = findFrameSlot(receiver_.frameId());
  if (slot < 0) slot = chooseFrameSlot(receiver_.frameId());
  if (slot < 0) return false;

  if ((flags & ble_terminal::FRAME_FLAG_LATEST) != 0) {
    for (auto& frame : frames_) frame.latest = false;
  }

  CachedFrame& frame = frames_[slot];
  frame.id = receiver_.frameId();
  frame.length = receiver_.frameLength();
  frame.occupied = true;
  frame.latest = (flags & ble_terminal::FRAME_FLAG_LATEST) != 0;
  std::memcpy(frame.text.data(), receiver_.frameText(), frame.length + 1);

  const bool explicitlyPresented = (flags & ble_terminal::FRAME_FLAG_PRESENT) != 0;
  const bool shouldPresent = selectedSlot_ < 0 || explicitlyPresented || (frame.latest && followLatest_);
  if (shouldPresent) {
    selectedSlot_ = static_cast<int8_t>(slot);
    followLatest_ = frame.latest;
    if (explicitlyPresented && cleanRequestedFrame_) cleanRefreshPending_ = true;
    if (explicitlyPresented) cleanRequestedFrame_ = false;
  }
  needsReset_ = false;
  return shouldPresent;
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

    const bool sent = keyboardResult->text.empty() ? transport_.sendAction(ble_terminal::Action::SUBMIT_INPUT)
                                                   : transport_.sendCommand(keyboardResult->text);
    if (!sent) {
      commandSendFailed_ = true;
      requestUpdate();
    }
  });
}

int BleTerminalActivity::terminalFontId() const { return TERMINAL_FONT_IDS[fontSizeIndex_]; }

void BleTerminalActivity::changeFontSize(const int direction) {
  const int nextIndex =
      std::clamp(static_cast<int>(fontSizeIndex_) + direction, 0, static_cast<int>(TERMINAL_FONT_IDS.size()) - 1);
  if (nextIndex == fontSizeIndex_) return;
  fontSizeIndex_ = static_cast<uint8_t>(nextIndex);
  requestUpdate();
}

void BleTerminalActivity::queueFrameRequest(const ble_terminal::FrameRequest request, const uint32_t anchorFrameId,
                                            const bool cleanRefresh) {
  pendingFrameRequest_ = request;
  pendingRequestAnchor_ = anchorFrameId;
  hasPendingRequest_ = true;
  cleanRequestedFrame_ = cleanRequestedFrame_ || cleanRefresh;
  trySendPendingControl();
}

void BleTerminalActivity::trySendPendingControl() {
  const unsigned long now = millis();
  if (now < nextControlAttemptAt_ || !transport_.readyToSend()) return;

  bool sent = false;
  if (hasPendingStatus_) {
    sent = transport_.sendFrameStatus(pendingStatusFrameId_, pendingFrameStatus_);
    if (sent) hasPendingStatus_ = false;
  } else if (hasPendingRequest_) {
    sent = transport_.sendFrameRequest(pendingFrameRequest_, pendingRequestAnchor_);
    if (sent) hasPendingRequest_ = false;
  }
  nextControlAttemptAt_ = now + (sent ? CONTROL_SETTLE_MS : CONTROL_RETRY_MS);
}

void BleTerminalActivity::navigateFrame(const int direction) {
  uint32_t anchor = 0;
  bool changed = false;
  bool requestRemote = false;

  if (frameMutex_ && xSemaphoreTake(frameMutex_, portMAX_DELAY) == pdTRUE) {
    anchor = selectedFrameId();
    if (anchor == 0) {
      requestRemote = true;
    } else {
      const uint32_t expected = direction < 0 ? anchor - 1U : anchor + 1U;
      const int slot = findFrameSlot(expected);
      if (slot >= 0) {
        selectedSlot_ = static_cast<int8_t>(slot);
        followLatest_ = frames_[slot].latest;
        changed = true;
      } else if (direction < 0 || !frames_[selectedSlot_].latest) {
        requestRemote = true;
      }
    }
    xSemaphoreGive(frameMutex_);
  }

  if (changed) requestUpdate();
  if (requestRemote) {
    queueFrameRequest(direction < 0 ? ble_terminal::FrameRequest::PREVIOUS : ble_terminal::FrameRequest::NEXT, anchor);
  }
}

void BleTerminalActivity::jumpToLatest() {
  bool changed = false;
  uint32_t anchor = 0;
  if (frameMutex_ && xSemaphoreTake(frameMutex_, portMAX_DELAY) == pdTRUE) {
    for (size_t index = 0; index < frames_.size(); ++index) {
      if (frames_[index].occupied && frames_[index].latest) {
        changed = selectedSlot_ != static_cast<int>(index);
        selectedSlot_ = static_cast<int8_t>(index);
        followLatest_ = true;
        break;
      }
    }
    anchor = selectedFrameId();
    xSemaphoreGive(frameMutex_);
  }
  if (changed) requestUpdate();
  queueFrameRequest(ble_terminal::FrameRequest::CURRENT, anchor);
}

void BleTerminalActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    LOG_INF("BLE_TERM", "Back requested: stopping transport before Home");
    transport_.stop();
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

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    queueFrameRequest(ble_terminal::FrameRequest::CURRENT, selectedFrameId(), true);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    queueFrameRequest(ble_terminal::FrameRequest::CURRENT, selectedFrameId());
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Down, LONG_PRESS_MS)) {
    jumpToLatest();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    navigateFrame(-1);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    navigateFrame(1);
  }

  const unsigned long now = millis();
  ble_terminal::BleTerminalTransport::IncomingPacket packet;
  while (transport_.poll(packet)) {
    if (!frameMutex_ || xSemaphoreTake(frameMutex_, portMAX_DELAY) != pdTRUE) continue;

    const ble_terminal::AcceptResult result = receiver_.accept(packet.bytes.data(), packet.length);
    const uint32_t frameId = receiver_.frameId();
    const bool validFramePacket = result == ble_terminal::AcceptResult::FRAME_STARTED ||
                                  result == ble_terminal::AcceptResult::FRAME_DATA_ACCEPTED ||
                                  result == ble_terminal::AcceptResult::FRAME_COMMITTED ||
                                  result == ble_terminal::AcceptResult::DUPLICATE_IGNORED;
    const bool frameError =
        result == ble_terminal::AcceptResult::NEEDS_BEGIN || result == ble_terminal::AcceptResult::OUT_OF_ORDER ||
        result == ble_terminal::AcceptResult::TOO_LARGE || result == ble_terminal::AcceptResult::INVALID_TEXT ||
        result == ble_terminal::AcceptResult::INVALID_FRAME || result == ble_terminal::AcceptResult::CRC_MISMATCH ||
        result == ble_terminal::AcceptResult::UNSUPPORTED_VERSION;

    bool presentFrame = false;
    if (result == ble_terminal::AcceptResult::FRAME_COMMITTED) {
      presentFrame = cacheCommittedFrame();
    }
    if (frameError) needsReset_ = true;
    xSemaphoreGive(frameMutex_);

    if (validFramePacket || frameError) {
      lastPacketAt_ = now;
      lastTransferActivityAt_ = now;
      transport_.setTransferActive(true);
    }
    if (result == ble_terminal::AcceptResult::FRAME_COMMITTED) {
      if (presentFrame) {
        readyAfterRenderFrameId_ = frameId;
        requestUpdate();
      } else {
        pendingStatusFrameId_ = frameId;
        pendingFrameStatus_ = ble_terminal::FrameStatus::READY;
        hasPendingStatus_ = true;
      }
    } else if (frameError && frameId != 0) {
      pendingStatusFrameId_ = frameId;
      pendingFrameStatus_ = ble_terminal::FrameStatus::RETRY;
      hasPendingStatus_ = true;
      requestUpdate();
    }
  }

  const uint32_t statusRevision = transport_.statusRevision();
  if (statusRevision != observedStatusRevision_) {
    observedStatusRevision_ = statusRevision;
    const auto status = transport_.status();
    if (status == ble_terminal::BleTerminalTransport::Status::CONNECTED) {
      lastTransferActivityAt_ = now;
      transport_.setTransferActive(true);
    } else {
      initialFrameRequested_ = false;
    }
    if (selectedSlot_ < 0 || status == ble_terminal::BleTerminalTransport::Status::PAIRING ||
        status == ble_terminal::BleTerminalTransport::Status::ERROR) {
      requestUpdate();
    }
  }

  if (!initialFrameRequested_ && transport_.status() == ble_terminal::BleTerminalTransport::Status::CONNECTED &&
      transport_.readyToSend()) {
    initialFrameRequested_ = true;
    queueFrameRequest(ble_terminal::FrameRequest::CURRENT, selectedFrameId());
  }

  if (lastTransferActivityAt_ != 0 && now - lastTransferActivityAt_ >= TRANSFER_IDLE_MS) {
    lastTransferActivityAt_ = 0;
    transport_.setTransferActive(false);
  }
  trySendPendingControl();
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

  if (frameMutex_ && xSemaphoreTake(frameMutex_, portMAX_DELAY) == pdTRUE) {
    const auto status = transport_.status();
    const bool securityStatusNeedsScreen = status == ble_terminal::BleTerminalTransport::Status::PAIRING ||
                                           status == ble_terminal::BleTerminalTransport::Status::ERROR;
    const bool hasFrame = selectedSlot_ >= 0 && frames_[selectedSlot_].occupied;
    if (commandSendFailed_ || needsReset_ || !hasFrame || maxLines <= 0 || securityStatusNeedsScreen) {
      std::array<char, 192> statusMessage{};
      formatStatusText(statusMessage.data(), statusMessage.size());
      const uint32_t pairingPasskey = transport_.pairingPasskey();
      if (status == ble_terminal::BleTerminalTransport::Status::PAIRING && pairingPasskey != 0) {
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
        UITheme::drawCenteredWrappedText(renderer, Rect{metrics.contentSidePadding, bodyTop, bodyWidth, bodyHeight},
                                         UI_10_FONT_ID, statusMessage.data(), 5);
      }
    } else {
      const CachedFrame& frame = frames_[selectedSlot_];
      const size_t totalLines =
          countVisualLines(renderer, fontId, frame.text.data(), frame.length, bodyWidth, displayLine_);
      const size_t visibleLines = static_cast<size_t>(maxLines);
      const size_t firstIndex = totalLines > visibleLines ? totalLines - visibleLines : 0;
      size_t cursor =
          visualLineStart(renderer, fontId, frame.text.data(), frame.length, bodyWidth, firstIndex, displayLine_);
      int y = bodyTop;
      for (int displayedLine = 0; displayedLine < maxLines && cursor <= frame.length; ++displayedLine) {
        const VisualLine visualLine =
            prepareVisualLine(renderer, fontId, frame.text.data(), frame.length, cursor, bodyWidth, displayLine_);
        if (displayLine_[0] != '\0') renderer.drawText(fontId, metrics.contentSidePadding, y, displayLine_.data());
        y += lineHeight;
        cursor = visualLine.nextStart;
      }
    }
    xSemaphoreGive(frameMutex_);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_FORCE_REFRESH), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  const HalDisplay::RefreshMode refreshMode =
      cleanRefreshPending_ ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
  cleanRefreshPending_ = false;
  renderer.displayBuffer(refreshMode);

  if (readyAfterRenderFrameId_ != 0) {
    pendingStatusFrameId_ = readyAfterRenderFrameId_;
    pendingFrameStatus_ = ble_terminal::FrameStatus::READY;
    hasPendingStatus_ = true;
    readyAfterRenderFrameId_ = 0;
  }
}

#endif
