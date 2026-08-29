#pragma once

#if defined(ENABLE_BLE_TERMINAL) && ENABLE_BLE_TERMINAL

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "BleTerminalProtocol.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace ble_terminal {

class BleTerminalTransport final {
 public:
  enum class Status : uint8_t { STOPPED, STARTING, ADVERTISING, PAIRING, CONNECTED, ERROR };

  struct IncomingPacket {
    uint16_t length = 0;
    std::array<uint8_t, MAX_PACKET_BYTES> bytes{};
  };

  BleTerminalTransport();
  ~BleTerminalTransport();

  bool start();
  void stop();
  bool poll(IncomingPacket& packet);
  bool sendAction(Action action);
  bool sendCommand(const std::string& command);
  bool readyToSend() const;
  size_t maxCommandBytes() const;
  void setTransferActive(bool active);

  Status status() const { return status_.load(); }
  uint32_t statusRevision() const { return statusRevision_.load(); }
  uint32_t droppedPackets() const { return droppedPackets_.load(); }
  uint32_t pairingPasskey() const { return pairingPasskey_.load(); }

 private:
  static constexpr size_t QUEUE_DEPTH = 8;
  static constexpr uint16_t NO_CONNECTION = 0xFFFF;

  StaticQueue_t queueControl_{};
  std::array<uint8_t, QUEUE_DEPTH * sizeof(IncomingPacket)> queueStorage_{};
  QueueHandle_t queue_ = nullptr;

  std::atomic<Status> status_{Status::STOPPED};
  std::atomic<uint32_t> statusRevision_{0};
  std::atomic<uint32_t> droppedPackets_{0};
  std::atomic<uint32_t> pairingPasskey_{0};
  std::atomic<uint16_t> connectionHandle_{NO_CONNECTION};
  std::atomic<bool> indicationsEnabled_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> transferActive_{false};
  bool stackInitialized_ = false;
  bool hostTaskStarted_ = false;
  uint8_t ownAddressType_ = 0;
  uint16_t txValueHandle_ = 0;
  uint32_t outgoingSequence_ = 0;

  static std::atomic<BleTerminalTransport*> active_;

  void setStatus(Status status);
  void setPairingPasskey(uint32_t passkey);
  bool configureGatt();
  bool requestConnectionParameters(uint16_t connectionHandle, bool active);
  static bool isConnectionSecure(uint16_t connectionHandle);
  bool sendPacket(const uint8_t* packet, size_t length, const char* description);
  int startAdvertising();
  bool failStart(const char* reason, int errorCode);
  static void hostTask(void* parameter);
  static void onReset(int reason);
  static void onSync();
  static int gapEvent(ble_gap_event* event, void* argument);
  static int gattAccess(uint16_t connectionHandle, uint16_t attributeHandle, ble_gatt_access_ctxt* context,
                        void* argument);
};

// NimBLE finishes its FreeRTOS host task asynchronously after
// nimble_port_stop(). Keep the transport alive across activity destruction so
// a late host-task epilogue can never touch a freed BleTerminalActivity.
BleTerminalTransport& sharedTransport();

}  // namespace ble_terminal

#endif
