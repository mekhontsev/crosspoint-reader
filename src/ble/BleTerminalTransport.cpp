#include "BleTerminalTransport.h"

#if defined(ENABLE_BLE_TERMINAL) && ENABLE_BLE_TERMINAL

#include <Arduino.h>
#include <Logging.h>
#include <esp_err.h>
#include <esp_random.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_sm.h>
#include <host/ble_store.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

// ESP-IDF's persistent NimBLE store intentionally has no public declaration.
// This is the same declaration used by the official NimBLE Security example.
extern "C" void ble_store_config_init(void);

namespace ble_terminal {
namespace {

constexpr char DEVICE_NAME[] = "X4 Terminal";
constexpr uint16_t CONNECTION_INTERVAL_MIN = 80;   // 100 ms in 1.25 ms units
constexpr uint16_t CONNECTION_INTERVAL_MAX = 160;  // 200 ms in 1.25 ms units
constexpr uint16_t CONNECTION_LATENCY = 4;
constexpr uint16_t SUPERVISION_TIMEOUT = 600;  // 6 seconds in 10 ms units
constexpr uint16_t ADVERTISING_INTERVAL_MIN = 800;   // 500 ms in 0.625 ms units
constexpr uint16_t ADVERTISING_INTERVAL_MAX = 1200;  // 750 ms in 0.625 ms units
constexpr size_t LEGACY_ADVERTISING_MAX_BYTES = 31;
constexpr size_t FLAGS_FIELD_BYTES = 3;
constexpr size_t UUID128_FIELD_BYTES = 18;
constexpr size_t DEVICE_NAME_FIELD_BYTES = 2 + sizeof(DEVICE_NAME) - 1;

static_assert(FLAGS_FIELD_BYTES + UUID128_FIELD_BYTES <= LEGACY_ADVERTISING_MAX_BYTES);
static_assert(DEVICE_NAME_FIELD_BYTES <= LEGACY_ADVERTISING_MAX_BYTES);

// UUIDs are stored least-significant byte first by NimBLE.
const ble_uuid128_t SERVICE_UUID =
    BLE_UUID128_INIT(0x01, 0x6c, 0x3f, 0x7b, 0x8b, 0x8e, 0xa6, 0xa2, 0x1e, 0x4f, 0x7a, 0x7f, 0x10, 0x8f, 0x2c, 0x6f);
const ble_uuid128_t RX_UUID =
    BLE_UUID128_INIT(0x02, 0x6c, 0x3f, 0x7b, 0x8b, 0x8e, 0xa6, 0xa2, 0x1e, 0x4f, 0x7a, 0x7f, 0x10, 0x8f, 0x2c, 0x6f);
const ble_uuid128_t TX_UUID =
    BLE_UUID128_INIT(0x03, 0x6c, 0x3f, 0x7b, 0x8b, 0x8e, 0xa6, 0xa2, 0x1e, 0x4f, 0x7a, 0x7f, 0x10, 0x8f, 0x2c, 0x6f);

ble_gatt_chr_def characteristics[3]{};
ble_gatt_svc_def services[2]{};

}  // namespace

static_assert(std::is_trivially_copyable_v<BleTerminalTransport::IncomingPacket>);

std::atomic<BleTerminalTransport*> BleTerminalTransport::active_{nullptr};

BleTerminalTransport::BleTerminalTransport() {
  queue_ = xQueueCreateStatic(QUEUE_DEPTH, sizeof(IncomingPacket), queueStorage_.data(), &queueControl_);
}

BleTerminalTransport::~BleTerminalTransport() { stop(); }

void BleTerminalTransport::setStatus(const Status status) {
  if (status_.exchange(status) != status) statusRevision_++;
}

void BleTerminalTransport::setPairingPasskey(const uint32_t passkey) {
  if (pairingPasskey_.exchange(passkey) != passkey) statusRevision_++;
}

bool BleTerminalTransport::failStart(const char* reason, const int errorCode) {
  LOG_ERR("BLE_TERM", "%s (%d)", reason, errorCode);
  stop();
  setStatus(Status::ERROR);
  return false;
}

bool BleTerminalTransport::configureGatt() {
  std::memset(characteristics, 0, sizeof(characteristics));
  std::memset(services, 0, sizeof(services));

  characteristics[0].uuid = &RX_UUID.u;
  characteristics[0].access_cb = &BleTerminalTransport::gattAccess;
  characteristics[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;

  characteristics[1].uuid = &TX_UUID.u;
  characteristics[1].access_cb = &BleTerminalTransport::gattAccess;
  characteristics[1].flags = BLE_GATT_CHR_F_INDICATE;
  characteristics[1].val_handle = &txValueHandle_;

  services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  services[0].uuid = &SERVICE_UUID.u;
  services[0].characteristics = characteristics;

  int result = ble_gatts_count_cfg(services);
  if (result != 0) {
    LOG_ERR("BLE_TERM", "Unable to count GATT service (%d)", result);
    return false;
  }
  result = ble_gatts_add_svcs(services);
  if (result != 0) {
    LOG_ERR("BLE_TERM", "Unable to add GATT service (%d)", result);
    return false;
  }
  return true;
}

bool BleTerminalTransport::start() {
  if (status() != Status::STOPPED && status() != Status::ERROR) return true;
  if (!queue_) return failStart("Packet queue is unavailable", ESP_ERR_NO_MEM);

  xQueueReset(queue_);
  droppedPackets_ = 0;
  pairingPasskey_ = 0;
  connectionHandle_ = NO_CONNECTION;
  indicationsEnabled_ = false;
  txValueHandle_ = 0;
  outgoingSequence_ = 0;
  stopping_.store(false);
  active_.store(this);
  setStatus(Status::STARTING);

  LOG_INF("BLE_TERM", "Initializing NimBLE port");
  const esp_err_t initResult = nimble_port_init();
  if (initResult != ESP_OK) return failStart("NimBLE initialization failed", initResult);
  stackInitialized_ = true;
  LOG_INF("BLE_TERM", "NimBLE port initialized");

  constexpr uint16_t PREFERRED_ATT_MTU = 247;
  const int mtuResult = ble_att_set_preferred_mtu(PREFERRED_ATT_MTU);
  if (mtuResult != 0) return failStart("Unable to set preferred ATT MTU", mtuResult);

  ble_hs_cfg.reset_cb = &BleTerminalTransport::onReset;
  ble_hs_cfg.sync_cb = &BleTerminalTransport::onSync;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_sc_only = 1;
  ble_hs_cfg.sm_sec_lvl = 4;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_store_config_init();

  ble_svc_gap_init();
  ble_svc_gatt_init();
  int result = ble_svc_gap_device_name_set(DEVICE_NAME);
  if (result != 0) return failStart("Unable to set BLE device name", result);
  if (!configureGatt()) return failStart("Unable to configure GATT", ESP_FAIL);
  LOG_INF("BLE_TERM", "GATT services queued");

  hostTaskStarted_ = true;
  nimble_port_freertos_init(&BleTerminalTransport::hostTask);
  LOG_INF("BLE_TERM", "NimBLE host task launched");

  constexpr int SYNC_WAIT_STEPS = 200;
  for (int step = 0; step < SYNC_WAIT_STEPS; ++step) {
    const Status current = status();
    if (current == Status::ADVERTISING || current == Status::CONNECTED) {
      LOG_INF("BLE_TERM", "NimBLE synchronized");
      return true;
    }
    if (current == Status::ERROR) return failStart("NimBLE host failed to synchronize", ESP_FAIL);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return failStart("NimBLE host synchronization timed out", ESP_ERR_TIMEOUT);
}

void BleTerminalTransport::stop() {
  if (!stackInitialized_ && !hostTaskStarted_) {
    BleTerminalTransport* expected = this;
    active_.compare_exchange_strong(expected, nullptr);
    setStatus(Status::STOPPED);
    return;
  }

  stopping_.store(true);
  ble_gap_adv_stop();
  const uint16_t connectionHandle = connectionHandle_.load();
  if (connectionHandle != NO_CONNECTION) ble_gap_terminate(connectionHandle, BLE_ERR_REM_USER_CONN_TERM);

  if (hostTaskStarted_) {
    const int stopResult = nimble_port_stop();
    if (stopResult != 0) LOG_ERR("BLE_TERM", "NimBLE stop failed (%d)", stopResult);

    constexpr int HOST_STOP_WAIT_STEPS = 100;
    for (int step = 0; hostTaskRunning_.load() && step < HOST_STOP_WAIT_STEPS; ++step) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  if (stackInitialized_) {
    const esp_err_t deinitResult = nimble_port_deinit();
    if (deinitResult != ESP_OK) LOG_ERR("BLE_TERM", "NimBLE deinit failed (%d)", deinitResult);
  }

  BleTerminalTransport* expected = this;
  active_.compare_exchange_strong(expected, nullptr);
  stackInitialized_ = false;
  hostTaskStarted_ = false;
  hostTaskRunning_ = false;
  connectionHandle_ = NO_CONNECTION;
  indicationsEnabled_ = false;
  setPairingPasskey(0);
  stopping_.store(false);
  if (queue_) xQueueReset(queue_);
  setStatus(Status::STOPPED);
}

bool BleTerminalTransport::poll(IncomingPacket& packet) {
  return queue_ && xQueueReceive(queue_, &packet, 0) == pdPASS;
}

bool BleTerminalTransport::sendAction(const Action action) {
  std::array<uint8_t, PACKET_HEADER_BYTES + 1> packet{};
  const size_t length = encodeActionPacket(action, outgoingSequence_, packet.data(), packet.size());
  if (length == 0) return false;
  return sendPacket(packet.data(), length, "reader action");
}

bool BleTerminalTransport::sendCommand(const std::string& command) {
  std::array<uint8_t, MAX_PACKET_BYTES> packet{};
  const size_t length = encodeCommandPacket(reinterpret_cast<const uint8_t*>(command.data()), command.size(),
                                            outgoingSequence_, packet.data(), packet.size());
  if (length == 0 || command.size() > maxCommandBytes()) return false;
  return sendPacket(packet.data(), length, "reader command");
}

bool BleTerminalTransport::readyToSend() const {
  return status() == Status::CONNECTED && connectionHandle_.load() != NO_CONNECTION && txValueHandle_ != 0 &&
         indicationsEnabled_.load();
}

size_t BleTerminalTransport::maxCommandBytes() const {
  const uint16_t connectionHandle = connectionHandle_.load();
  if (!readyToSend() || connectionHandle == NO_CONNECTION) return 0;

  const uint16_t mtu = ble_att_mtu(connectionHandle);
  if (mtu <= PACKET_HEADER_BYTES + 3) return 0;
  return std::min(MAX_COMMAND_BYTES, static_cast<size_t>(mtu - 3 - PACKET_HEADER_BYTES));
}

bool BleTerminalTransport::sendPacket(const uint8_t* packet, const size_t length, const char* description) {
  const uint16_t connectionHandle = connectionHandle_.load();
  const uint16_t mtu = connectionHandle == NO_CONNECTION ? 0 : ble_att_mtu(connectionHandle);
  if (!packet || length == 0 || !readyToSend() || !isConnectionSecure(connectionHandle) || mtu <= 3 ||
      length > static_cast<size_t>(mtu - 3)) {
    return false;
  }

  os_mbuf* value = ble_hs_mbuf_from_flat(packet, length);
  if (!value) return false;
  const int result = ble_gatts_indicate_custom(connectionHandle, txValueHandle_, value);
  if (result != 0) {
    LOG_ERR("BLE_TERM", "Unable to send %s (%d)", description, result);
    return false;
  }
  outgoingSequence_++;
  return true;
}

bool BleTerminalTransport::isConnectionSecure(const uint16_t connectionHandle) {
  ble_gap_conn_desc descriptor{};
  const int result = ble_gap_conn_find(connectionHandle, &descriptor);
  return result == 0 && descriptor.sec_state.encrypted && descriptor.sec_state.authenticated;
}

void BleTerminalTransport::hostTask(void*) {
  BleTerminalTransport* instance = active_.load();
  if (instance) instance->hostTaskRunning_ = true;
  nimble_port_run();
  if (instance) instance->hostTaskRunning_ = false;
  nimble_port_freertos_deinit();
}

void BleTerminalTransport::onReset(const int reason) {
  LOG_ERR("BLE_TERM", "NimBLE reset (%d)", reason);
  BleTerminalTransport* instance = active_.load();
  if (instance && !instance->stopping_.load()) instance->setStatus(Status::STARTING);
}

void BleTerminalTransport::onSync() {
  BleTerminalTransport* instance = active_.load();
  if (!instance || instance->stopping_.load()) return;

  const int addressResult = ble_hs_id_infer_auto(0, &instance->ownAddressType_);
  if (addressResult != 0) {
    LOG_ERR("BLE_TERM", "Unable to select BLE address (%d)", addressResult);
    instance->setStatus(Status::ERROR);
    return;
  }

  const int advertisingResult = instance->startAdvertising();
  if (advertisingResult != 0) {
    LOG_ERR("BLE_TERM", "Unable to start advertising (%d)", advertisingResult);
    instance->setStatus(Status::ERROR);
  }
}

int BleTerminalTransport::startAdvertising() {
  // A legacy advertising packet is limited to 31 bytes. Flags, the complete
  // 128-bit service UUID, and "X4 Terminal" need 34 bytes together, so keep
  // the discoverable service UUID in the primary packet and put the name in
  // the scan response.
  ble_hs_adv_fields advertisingFields{};
  advertisingFields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  advertisingFields.uuids128 = const_cast<ble_uuid128_t*>(&SERVICE_UUID);
  advertisingFields.num_uuids128 = 1;
  advertisingFields.uuids128_is_complete = 1;

  int result = ble_gap_adv_set_fields(&advertisingFields);
  if (result != 0) return result;

  ble_hs_adv_fields scanResponseFields{};
  scanResponseFields.name = reinterpret_cast<const uint8_t*>(DEVICE_NAME);
  scanResponseFields.name_len = sizeof(DEVICE_NAME) - 1;
  scanResponseFields.name_is_complete = 1;

  result = ble_gap_adv_rsp_set_fields(&scanResponseFields);
  if (result != 0) return result;

  ble_gap_adv_params parameters{};
  parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
  parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
  parameters.itvl_min = ADVERTISING_INTERVAL_MIN;
  parameters.itvl_max = ADVERTISING_INTERVAL_MAX;
  result = ble_gap_adv_start(ownAddressType_, nullptr, BLE_HS_FOREVER, &parameters, &BleTerminalTransport::gapEvent,
                             nullptr);
  if (result != 0) return result;
  setStatus(Status::ADVERTISING);
  return 0;
}

int BleTerminalTransport::gapEvent(ble_gap_event* event, void*) {
  BleTerminalTransport* instance = active_.load();
  if (!instance || !event) return 0;

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        instance->connectionHandle_ = event->connect.conn_handle;
        instance->indicationsEnabled_ = false;
        instance->setPairingPasskey(0);
        instance->setStatus(Status::PAIRING);

        const ble_gap_upd_params parameters{CONNECTION_INTERVAL_MIN, CONNECTION_INTERVAL_MAX, CONNECTION_LATENCY,
                                            SUPERVISION_TIMEOUT, 0, 0};
        const int updateResult = ble_gap_update_params(event->connect.conn_handle, &parameters);
        if (updateResult != 0) LOG_DBG("BLE_TERM", "Connection parameter request failed (%d)", updateResult);

        // The central initiates pairing explicitly. This lets clients register
        // the correct passkey-entry handler before the Security Manager asks
        // the reader to display a code. GATT permissions still reject every
        // terminal write until authenticated encryption is established.
      } else if (!instance->stopping_.load()) {
        const int result = instance->startAdvertising();
        if (result != 0) {
          LOG_ERR("BLE_TERM", "Unable to restart advertising after connection failure (%d)", result);
          instance->setStatus(Status::ERROR);
        }
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      instance->connectionHandle_ = NO_CONNECTION;
      instance->indicationsEnabled_ = false;
      instance->setPairingPasskey(0);
      if (!instance->stopping_.load()) {
        const int result = instance->startAdvertising();
        if (result != 0) {
          LOG_ERR("BLE_TERM", "Unable to restart advertising after disconnect (%d)", result);
          instance->setStatus(Status::ERROR);
        }
      }
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == instance->txValueHandle_) {
        const bool enabled = event->subscribe.cur_indicate != 0;
        if (instance->indicationsEnabled_.exchange(enabled) != enabled) instance->statusRevision_++;
        LOG_INF("BLE_TERM", "Reader input indications %s",
                instance->indicationsEnabled_.load() ? "enabled" : "disabled");
      }
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status == 0 && instance->isConnectionSecure(event->enc_change.conn_handle)) {
        LOG_INF("BLE_TERM", "Connection is encrypted and authenticated");
        instance->setPairingPasskey(0);
        instance->setStatus(Status::CONNECTED);
      } else {
        LOG_ERR("BLE_TERM", "Authenticated encryption failed (%d)", event->enc_change.status);
        instance->setPairingPasskey(0);
        ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      }
      break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      if (event->passkey.params.action != BLE_SM_IOACT_DISP) {
        LOG_ERR("BLE_TERM", "Unexpected pairing action (%u)", event->passkey.params.action);
        return BLE_HS_ENOTSUP;
      } else {
        ble_sm_io pairingIo{};
        pairingIo.action = BLE_SM_IOACT_DISP;
        pairingIo.passkey = 100000U + esp_random() % 900000U;
        const int result = ble_sm_inject_io(event->passkey.conn_handle, &pairingIo);
        if (result != 0) {
          LOG_ERR("BLE_TERM", "Unable to supply pairing passkey (%d)", result);
          return result;
        }
        instance->setPairingPasskey(pairingIo.passkey);
        instance->setStatus(Status::PAIRING);
        LOG_INF("BLE_TERM", "Pairing passkey displayed on reader");
      }
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      ble_gap_conn_desc descriptor{};
      const int findResult = ble_gap_conn_find(event->repeat_pairing.conn_handle, &descriptor);
      if (findResult != 0) {
        LOG_ERR("BLE_TERM", "Unable to find peer for repeat pairing (%d)", findResult);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
      }
      const int deleteResult = ble_store_util_delete_peer(&descriptor.peer_id_addr);
      if (deleteResult != 0) {
        LOG_ERR("BLE_TERM", "Unable to remove stale BLE bond (%d)", deleteResult);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
      }
      instance->setPairingPasskey(0);
      instance->setStatus(Status::PAIRING);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PARING_COMPLETE:
      if (event->pairing_complete.status != 0) {
        LOG_ERR("BLE_TERM", "Pairing failed (%d)", event->pairing_complete.status);
        instance->setPairingPasskey(0);
        ble_gap_terminate(event->pairing_complete.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      }
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (!instance->stopping_.load() && instance->status() != Status::CONNECTED) {
        const int result = instance->startAdvertising();
        if (result != 0) {
          LOG_ERR("BLE_TERM", "Unable to restart advertising after completion (%d)", result);
          instance->setStatus(Status::ERROR);
        }
      }
      break;
    default:
      break;
  }
  return 0;
}

int BleTerminalTransport::gattAccess(const uint16_t connectionHandle, const uint16_t, ble_gatt_access_ctxt* context,
                                     void*) {
  BleTerminalTransport* instance = active_.load();
  if (!instance || !context || context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  if (!isConnectionSecure(connectionHandle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

  const uint16_t length = OS_MBUF_PKTLEN(context->om);
  if (length == 0 || length > MAX_PACKET_BYTES) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

  IncomingPacket packet{};
  packet.length = length;
  uint16_t copied = 0;
  const int copyResult = ble_hs_mbuf_to_flat(context->om, packet.bytes.data(), packet.bytes.size(), &copied);
  if (copyResult != 0 || copied != length) return BLE_ATT_ERR_UNLIKELY;

  if (!instance->queue_ || xQueueSend(instance->queue_, &packet, 0) != pdPASS) {
    instance->droppedPackets_++;
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return 0;
}

}  // namespace ble_terminal

#endif
