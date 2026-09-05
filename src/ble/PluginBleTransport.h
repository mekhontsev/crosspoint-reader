#pragma once

#if defined(ENABLE_PLUGIN_BLE_HOST) && ENABLE_PLUGIN_BLE_HOST

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ConnectionModeRetry.h"
struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace plugin_ble {

// Authenticated raw packet transport shared by independently loaded plugins.

constexpr size_t MAX_PACKET_BYTES = 244;

class PluginBleTransport final {
 public:
  enum class Status : uint8_t { STOPPED, STARTING, ADVERTISING, PAIRING, CONNECTED, ERROR };

  struct IncomingPacket {
    uint16_t length = 0;
    std::array<uint8_t, MAX_PACKET_BYTES> bytes{};
  };

  PluginBleTransport();
  ~PluginBleTransport();

  bool start();
  void stop();
  bool poll(IncomingPacket& packet);
  bool send(const uint8_t* packet, size_t length);
  bool readyToSend() const;
  bool pollService(IncomingPacket& packet);
  bool sendService(const uint8_t* packet, size_t length);
  bool serviceReady() const;
  size_t maxPacketBytes() const;
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
  StaticQueue_t serviceQueueControl_{};
  std::array<uint8_t, 2 * sizeof(IncomingPacket)> serviceQueueStorage_{};
  QueueHandle_t serviceQueue_ = nullptr;

  std::atomic<Status> status_{Status::STOPPED};
  std::atomic<uint32_t> statusRevision_{0};
  std::atomic<uint32_t> droppedPackets_{0};
  std::atomic<uint32_t> pairingPasskey_{0};
  std::atomic<uint16_t> connectionHandle_{NO_CONNECTION};
  std::atomic<bool> indicationsEnabled_{false};
  std::atomic<bool> serviceIndicationsEnabled_{false};
  std::atomic<bool> indicationInFlight_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> transferActive_{false};
  uint16_t parameterConnectionHandle_ = NO_CONNECTION;
  bool parameterModeActive_ = false;
  ConnectionModeRetry parameterRetry_;
  bool stackInitialized_ = false;
  bool hostTaskStarted_ = false;
  uint8_t ownAddressType_ = 0;
  uint16_t txValueHandle_ = 0;
  uint16_t serviceTxValueHandle_ = 0;
  static std::atomic<PluginBleTransport*> active_;

  void setStatus(Status status);
  void setPairingPasskey(uint32_t passkey);
  bool configureGatt();
  bool sendOn(uint16_t handle, bool enabled, const uint8_t* packet, size_t length);
  bool requestConnectionParameters(uint16_t connectionHandle, bool active);
  void serviceConnectionParameters();
  static bool isConnectionSecure(uint16_t connectionHandle);
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
// a late host-task epilogue can never touch a freed plugin activity.
PluginBleTransport& sharedPluginBleTransport();

}  // namespace plugin_ble

#endif
