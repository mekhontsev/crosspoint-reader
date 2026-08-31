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
  static constexpr uint32_t DATA_KEEP_AWAKE_MS = 5000;
  static constexpr uint32_t TRANSFER_IDLE_MS = 2000;
  static constexpr size_t DISPLAY_LINE_BYTES = 256;
  static constexpr size_t FRAME_CACHE_SLOTS = 4;

  struct CachedFrame {
    std::array<char, ble_terminal::MAX_FRAME_BYTES + 1> text{};
    uint32_t id = 0;
    size_t length = 0;
    bool occupied = false;
    bool latest = false;
  };

  ble_terminal::BleTerminalTransport& transport_;
  std::array<CachedFrame, FRAME_CACHE_SLOTS> frames_{};
  std::array<char, ble_terminal::MAX_FRAME_BYTES + 1> stagingFrame_{};
  std::array<char, DISPLAY_LINE_BYTES> displayLine_{};
  ble_terminal::TextFrameReceiver receiver_;
  StaticSemaphore_t frameMutexStorage_{};
  SemaphoreHandle_t frameMutex_ = nullptr;

  uint32_t observedStatusRevision_ = 0;
  uint32_t pendingStatusFrameId_ = 0;
  uint32_t pendingRequestAnchor_ = 0;
  uint32_t readyAfterRenderFrameId_ = 0;
  unsigned long lastPacketAt_ = 0;
  unsigned long lastTransferActivityAt_ = 0;
  unsigned long nextControlAttemptAt_ = 0;
  int8_t selectedSlot_ = -1;
  uint8_t fontSizeIndex_ = 3;
  ble_terminal::FrameStatus pendingFrameStatus_ = ble_terminal::FrameStatus::READY;
  ble_terminal::FrameRequest pendingFrameRequest_ = ble_terminal::FrameRequest::CURRENT;
  bool hasPendingStatus_ = false;
  bool hasPendingRequest_ = false;
  bool initialFrameRequested_ = false;
  bool needsReset_ = false;
  bool commandSendFailed_ = false;
  bool followLatest_ = true;
  bool cleanRequestedFrame_ = false;
  bool cleanRefreshPending_ = false;

  void formatStatusText(char* buffer, size_t bufferSize) const;
  void openCommandKeyboard();
  int terminalFontId() const;
  void changeFontSize(int direction);
  void navigateFrame(int direction);
  void jumpToLatest();
  void queueFrameRequest(ble_terminal::FrameRequest request, uint32_t anchorFrameId, bool cleanRefresh = false);
  void trySendPendingControl();
  bool cacheCommittedFrame();
  int findFrameSlot(uint32_t frameId) const;
  int chooseFrameSlot(uint32_t frameId) const;
  uint32_t selectedFrameId() const;
  void clearFrameCache();
};

#endif
