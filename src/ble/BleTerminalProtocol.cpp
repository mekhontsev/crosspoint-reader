#include "BleTerminalProtocol.h"

#include <cstring>

namespace ble_terminal {
namespace {

constexpr uint8_t MAGIC_0 = 'P';
constexpr uint8_t MAGIC_1 = 'W';
constexpr size_t FRAME_BEGIN_PAYLOAD_BYTES = 11;
constexpr size_t FRAME_ID_PAYLOAD_BYTES = sizeof(uint32_t);
constexpr size_t FRAME_CONTROL_PAYLOAD_BYTES = sizeof(uint32_t) + sizeof(uint8_t);

static_assert(MAX_FRAME_BYTES <= UINT16_MAX);

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

void writeLe16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}

bool isAllowedAction(const Action action) {
  switch (action) {
    case Action::INTERRUPT_SESSION:
    case Action::APPROVE_REQUEST:
    case Action::REJECT_REQUEST:
    case Action::SUBMIT_INPUT:
      return true;
    default:
      return false;
  }
}

bool isAllowedFrameRequest(const FrameRequest request) {
  return request == FrameRequest::CURRENT || request == FrameRequest::PREVIOUS || request == FrameRequest::NEXT;
}

bool isAllowedFrameStatus(const FrameStatus status) {
  return status == FrameStatus::READY || status == FrameStatus::RETRY;
}

bool allowedCodepoint(const uint32_t cp) {
  if (cp == '\n' || cp == '\t') return true;
  return cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp <= 0x9F);
}

uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

size_t encodePacket(const PacketType type, const uint8_t* payload, const size_t payloadLength, const uint32_t sequence,
                    uint8_t* output, const size_t capacity) {
  const size_t packetLength = PACKET_HEADER_BYTES + payloadLength;
  if (!output || capacity < packetLength || packetLength > MAX_PACKET_BYTES) return 0;

  output[0] = MAGIC_0;
  output[1] = MAGIC_1;
  output[2] = PROTOCOL_VERSION;
  output[3] = static_cast<uint8_t>(type);
  writeLe32(output + 4, sequence);
  writeLe16(output + 8, static_cast<uint16_t>(payloadLength));
  if (payloadLength > 0) std::memcpy(output + PACKET_HEADER_BYTES, payload, payloadLength);
  return packetLength;
}

}  // namespace

bool decodePacket(const uint8_t* packet, const size_t length, PacketView& view) {
  if (!packet || length < PACKET_HEADER_BYTES || length > MAX_PACKET_BYTES || packet[0] != MAGIC_0 ||
      packet[1] != MAGIC_1 || packet[2] != PROTOCOL_VERSION) {
    return false;
  }
  const size_t payloadLength = readLe16(packet + 8);
  if (payloadLength != length - PACKET_HEADER_BYTES) return false;
  view.type = static_cast<PacketType>(packet[3]);
  view.sequence = readLe32(packet + 4);
  view.payload = packet + PACKET_HEADER_BYTES;
  view.payloadLength = payloadLength;
  return true;
}

bool isValidDisplayText(const uint8_t* data, const size_t length) {
  if (!data && length != 0) return false;

  size_t i = 0;
  while (i < length) {
    const uint8_t lead = data[i];
    uint32_t cp = 0;
    size_t bytes = 0;
    uint32_t minimum = 0;

    if (lead < 0x80) {
      cp = lead;
      bytes = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      cp = lead & 0x1FU;
      bytes = 2;
      minimum = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
      cp = lead & 0x0FU;
      bytes = 3;
      minimum = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
      cp = lead & 0x07U;
      bytes = 4;
      minimum = 0x10000;
    } else {
      return false;
    }

    if (i + bytes > length) return false;
    for (size_t continuation = 1; continuation < bytes; ++continuation) {
      const uint8_t value = data[i + continuation];
      if ((value & 0xC0U) != 0x80U) return false;
      cp = (cp << 6U) | (value & 0x3FU);
    }

    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || !allowedCodepoint(cp)) {
      return false;
    }
    i += bytes;
  }
  return true;
}

bool isValidCommandText(const uint8_t* data, const size_t length) {
  if (!data || length == 0 || length > MAX_COMMAND_BYTES || !isValidDisplayText(data, length)) return false;
  for (size_t i = 0; i < length; ++i) {
    if (data[i] == '\n' || data[i] == '\t') return false;
  }
  return true;
}

size_t encodeActionPacket(const Action action, const uint32_t sequence, uint8_t* output, const size_t capacity) {
  if (!isAllowedAction(action)) return 0;
  const uint8_t payload = static_cast<uint8_t>(action);
  return encodePacket(PacketType::ACTION, &payload, sizeof(payload), sequence, output, capacity);
}

size_t encodeCommandPacket(const uint8_t* command, const size_t commandLength, const uint32_t sequence, uint8_t* output,
                           const size_t capacity) {
  if (!isValidCommandText(command, commandLength)) return 0;
  return encodePacket(PacketType::COMMAND, command, commandLength, sequence, output, capacity);
}

size_t encodeFrameRequestPacket(const FrameRequest request, const uint32_t anchorFrameId, const uint32_t sequence,
                                uint8_t* output, const size_t capacity) {
  if (!isAllowedFrameRequest(request)) return 0;
  uint8_t payload[FRAME_CONTROL_PAYLOAD_BYTES]{};
  payload[0] = static_cast<uint8_t>(request);
  writeLe32(payload + 1, anchorFrameId);
  return encodePacket(PacketType::FRAME_REQUEST, payload, sizeof(payload), sequence, output, capacity);
}

size_t encodeFrameStatusPacket(const uint32_t frameId, const FrameStatus status, const uint32_t sequence,
                               uint8_t* output, const size_t capacity) {
  if (frameId == 0 || !isAllowedFrameStatus(status)) return 0;
  uint8_t payload[FRAME_CONTROL_PAYLOAD_BYTES]{};
  writeLe32(payload, frameId);
  payload[4] = static_cast<uint8_t>(status);
  return encodePacket(PacketType::FRAME_STATUS, payload, sizeof(payload), sequence, output, capacity);
}

size_t encodeViewportPacket(const uint16_t columns, const uint16_t rows, const uint32_t sequence, uint8_t* output,
                            const size_t capacity) {
  if (columns == 0 || rows == 0) return 0;
  uint8_t payload[sizeof(uint16_t) * 2]{};
  writeLe16(payload, columns);
  writeLe16(payload + sizeof(uint16_t), rows);
  return encodePacket(PacketType::VIEWPORT, payload, sizeof(payload), sequence, output, capacity);
}

size_t encodePluginUpdateHelloPacket(const uint32_t pluginAbi, const uint16_t maxDataBytes, const uint32_t sequence,
                                     uint8_t* output, const size_t capacity) {
  if (pluginAbi == 0 || maxDataBytes == 0 || maxDataBytes > MAX_UPDATE_DATA_BYTES) return 0;
  uint8_t payload[sizeof(uint32_t) + sizeof(uint16_t)]{};
  writeLe32(payload, pluginAbi);
  writeLe16(payload + sizeof(uint32_t), maxDataBytes);
  return encodePacket(PacketType::PLUGIN_UPDATE_HELLO, payload, sizeof(payload), sequence, output, capacity);
}

size_t encodePluginUpdateStatusPacket(const uint8_t status, const uint32_t value, const uint32_t sequence,
                                      uint8_t* output, const size_t capacity) {
  if (status == 0) return 0;
  uint8_t payload[sizeof(uint8_t) + sizeof(uint32_t)]{};
  payload[0] = status;
  writeLe32(payload + sizeof(uint8_t), value);
  return encodePacket(PacketType::PLUGIN_UPDATE_STATUS, payload, sizeof(payload), sequence, output, capacity);
}

TextFrameReceiver::TextFrameReceiver(char* buffer, const size_t capacity) : buffer_(buffer), capacity_(capacity) {
  if (buffer_ && capacity_ > 0) buffer_[0] = '\0';
}

void TextFrameReceiver::clear() {
  currentLength_ = 0;
  expectedLength_ = 0;
  expectedCrc_ = 0;
  frameId_ = 0;
  frameFlags_ = 0;
  beginSequence_ = 0;
  expectedSequence_ = 0;
  committedSequence_ = 0;
  committedFrameId_ = 0;
  receiving_ = false;
  hasCommittedFrame_ = false;
  if (buffer_ && capacity_ > 0) buffer_[0] = '\0';
}

AcceptResult TextFrameReceiver::accept(const uint8_t* packet, const size_t length) {
  if (!packet || !buffer_ || capacity_ == 0 || length < PACKET_HEADER_BYTES || length > MAX_PACKET_BYTES) {
    return AcceptResult::INVALID_PACKET;
  }
  if (packet[0] != MAGIC_0 || packet[1] != MAGIC_1) return AcceptResult::INVALID_PACKET;
  if (packet[2] != PROTOCOL_VERSION) return AcceptResult::UNSUPPORTED_VERSION;
  PacketView view{};
  if (!decodePacket(packet, length, view)) return AcceptResult::INVALID_PACKET;

  switch (view.type) {
    case PacketType::FRAME_BEGIN:
      return acceptBegin(view.sequence, view.payload, view.payloadLength);
    case PacketType::FRAME_DATA:
      return acceptData(view.sequence, view.payload, view.payloadLength);
    case PacketType::FRAME_COMMIT:
      return acceptCommit(view.sequence, view.payload, view.payloadLength);
    case PacketType::ACTION:
    case PacketType::COMMAND:
    case PacketType::FRAME_REQUEST:
    case PacketType::FRAME_STATUS:
    case PacketType::VIEWPORT:
    case PacketType::PLUGIN_UPDATE_HELLO:
    case PacketType::PLUGIN_UPDATE_BEGIN:
    case PacketType::PLUGIN_UPDATE_DATA:
    case PacketType::PLUGIN_UPDATE_END:
    case PacketType::PLUGIN_UPDATE_STATUS:
    default:
      return AcceptResult::UNEXPECTED_TYPE;
  }
}

AcceptResult TextFrameReceiver::acceptBegin(const uint32_t sequence, const uint8_t* payload,
                                            const size_t payloadLength) {
  if (!payload || payloadLength != FRAME_BEGIN_PAYLOAD_BYTES) return AcceptResult::INVALID_PACKET;

  const uint32_t frameId = readLe32(payload);
  const size_t frameLength = readLe16(payload + 4);
  const uint32_t frameCrc = readLe32(payload + 6);
  const uint8_t flags = payload[10];
  if (frameId == 0 || frameLength > MAX_FRAME_BYTES || frameLength + 1 > capacity_ ||
      (flags & ~FRAME_FLAGS_MASK) != 0) {
    receiving_ = false;
    return frameLength > MAX_FRAME_BYTES || frameLength + 1 > capacity_ ? AcceptResult::TOO_LARGE
                                                                        : AcceptResult::INVALID_FRAME;
  }

  if (receiving_ && sequence == beginSequence_ && frameId == frameId_ && frameLength == expectedLength_ &&
      frameCrc == expectedCrc_ && flags == frameFlags_) {
    return AcceptResult::DUPLICATE_IGNORED;
  }

  currentLength_ = 0;
  expectedLength_ = frameLength;
  expectedCrc_ = frameCrc;
  frameId_ = frameId;
  frameFlags_ = flags;
  beginSequence_ = sequence;
  expectedSequence_ = sequence + 1U;
  receiving_ = true;
  buffer_[0] = '\0';
  return AcceptResult::FRAME_STARTED;
}

AcceptResult TextFrameReceiver::acceptData(const uint32_t sequence, const uint8_t* payload,
                                           const size_t payloadLength) {
  if (!receiving_) return AcceptResult::NEEDS_BEGIN;
  if (sequence == expectedSequence_ - 1U) return AcceptResult::DUPLICATE_IGNORED;
  if (sequence != expectedSequence_) {
    receiving_ = false;
    return AcceptResult::OUT_OF_ORDER;
  }
  if (!payload || payloadLength == 0) return AcceptResult::INVALID_PACKET;
  if (currentLength_ + payloadLength > expectedLength_) {
    receiving_ = false;
    return AcceptResult::TOO_LARGE;
  }
  if (!isValidDisplayText(payload, payloadLength)) {
    receiving_ = false;
    return AcceptResult::INVALID_TEXT;
  }

  std::memcpy(buffer_ + currentLength_, payload, payloadLength);
  currentLength_ += payloadLength;
  buffer_[currentLength_] = '\0';
  expectedSequence_++;
  return AcceptResult::FRAME_DATA_ACCEPTED;
}

AcceptResult TextFrameReceiver::acceptCommit(const uint32_t sequence, const uint8_t* payload,
                                             const size_t payloadLength) {
  if (!payload || payloadLength != FRAME_ID_PAYLOAD_BYTES) return AcceptResult::INVALID_PACKET;
  const uint32_t commitFrameId = readLe32(payload);
  if (!receiving_) {
    if (hasCommittedFrame_ && sequence == committedSequence_ && commitFrameId == committedFrameId_) {
      return AcceptResult::DUPLICATE_IGNORED;
    }
    return AcceptResult::NEEDS_BEGIN;
  }
  if (sequence == expectedSequence_ - 1U) return AcceptResult::DUPLICATE_IGNORED;
  if (sequence != expectedSequence_) {
    receiving_ = false;
    return AcceptResult::OUT_OF_ORDER;
  }
  if (commitFrameId != frameId_ || currentLength_ != expectedLength_) {
    receiving_ = false;
    return AcceptResult::INVALID_FRAME;
  }
  if (crc32(reinterpret_cast<const uint8_t*>(buffer_), currentLength_) != expectedCrc_) {
    receiving_ = false;
    return AcceptResult::CRC_MISMATCH;
  }

  receiving_ = false;
  committedSequence_ = sequence;
  committedFrameId_ = frameId_;
  hasCommittedFrame_ = true;
  expectedSequence_++;
  return AcceptResult::FRAME_COMMITTED;
}

}  // namespace ble_terminal
