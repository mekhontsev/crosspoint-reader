#pragma once

#if defined(ENABLE_BLE_TERMINAL) && ENABLE_BLE_TERMINAL

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "activities/Activity.h"
#include "ble/BleTerminalProtocol.h"
#include "ble/BleTerminalTransport.h"

class BleTerminalActivity final : public Activity {
 public:
  BleTerminalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  static constexpr uint32_t MIN_DISPLAY_REFRESH_MS = 3000;
  static constexpr uint32_t STREAM_QUIET_MS = 700;
  static constexpr uint32_t MAX_DIRTY_LATENCY_MS = 3000;
  static constexpr uint32_t DATA_KEEP_AWAKE_MS = 5000;
  static constexpr uint32_t TRANSFER_IDLE_MS = 2000;
  static constexpr size_t DISPLAY_LINE_BYTES = 256;

  ble_terminal::BleTerminalTransport& transport_;
  std::array<char, ble_terminal::MAX_TRANSCRIPT_BYTES + 1> transcript_{};
  std::array<char, DISPLAY_LINE_BYTES> displayLine_{};
  ble_terminal::TextStreamReceiver receiver_;
  StaticSemaphore_t transcriptMutexStorage_{};
  SemaphoreHandle_t transcriptMutex_ = nullptr;

  uint32_t observedStatusRevision_ = 0;
  unsigned long dirtySince_ = 0;
  unsigned long lastPacketAt_ = 0;
  unsigned long lastTransferActivityAt_ = 0;
  unsigned long lastDisplayRequestAt_ = 0;
  bool screenDirty_ = false;
  bool needsReset_ = false;
  bool commandSendFailed_ = false;
  bool followTail_ = true;
  size_t viewportStart_ = 0;
  uint8_t fontSizeIndex_ = 3;

  void markScreenDirty(unsigned long now);
  void formatStatusText(char* buffer, size_t bufferSize) const;
  void openCommandKeyboard();
  int terminalFontId() const;
  void changeFontSize(int direction);
  void scrollPage(int direction);
  void jumpToTail();
};

#endif
