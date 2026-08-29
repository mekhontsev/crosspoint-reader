#include "BleTerminalProtocol.h"

#include <algorithm>
#include <cstring>

namespace ble_terminal {
namespace {

constexpr uint8_t MAGIC_0 = 'X';
constexpr uint8_t MAGIC_1 = 'T';
constexpr uint8_t PROTOCOL_VERSION = 1;

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
    case Action::PAGE_UP:
    case Action::PAGE_DOWN:
      return true;
    default:
      return false;
  }
}

bool allowedCodepoint(const uint32_t cp) {
  if (cp == '\n' || cp == '\t') return true;
  // Reject terminal control characters, including ESC and the C1 range. The
  // Android bridge must strip ANSI before constructing APPEND packets.
  return cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp <= 0x9F);
}

bool isUtf8Continuation(const char value) {
  return (static_cast<uint8_t>(value) & 0xC0U) == 0x80U;
}

}  // namespace

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
      minimum = 0;
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

  // A COMMAND represents one line. The bridge adds the terminal Enter key;
  // embedded line breaks and tabs would make that contract ambiguous.
  for (size_t i = 0; i < length; ++i) {
    if (data[i] == '\n' || data[i] == '\t') return false;
  }
  return true;
}

namespace {

size_t encodePacket(const PacketType type, const uint8_t* payload, const size_t payloadLength,
                    const uint32_t sequence, uint8_t* output, const size_t capacity) {
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

size_t encodeActionPacket(const Action action, const uint32_t sequence, uint8_t* output, const size_t capacity) {
  constexpr size_t ACTION_PACKET_BYTES = PACKET_HEADER_BYTES + 1;
  if (!output || capacity < ACTION_PACKET_BYTES || !isAllowedAction(action)) return 0;

  const uint8_t payload = static_cast<uint8_t>(action);
  return encodePacket(PacketType::ACTION, &payload, 1, sequence, output, capacity);
}

size_t encodeCommandPacket(const uint8_t* command, const size_t commandLength, const uint32_t sequence,
                           uint8_t* output, const size_t capacity) {
  if (!isValidCommandText(command, commandLength)) return 0;
  return encodePacket(PacketType::COMMAND, command, commandLength, sequence, output, capacity);
}

TextStreamReceiver::TextStreamReceiver(char* buffer, const size_t capacity)
    : buffer_(buffer),
      capacity_(capacity),
      maxTextBytes_(capacity > 0 ? std::min(capacity - 1, MAX_TRANSCRIPT_BYTES) : 0) {
  if (buffer_ && capacity_ > 0) buffer_[0] = '\0';
}

void TextStreamReceiver::clear() {
  currentLength_ = 0;
  lastDiscardedBytes_ = 0;
  expectedSequence_ = 0;
  synchronized_ = false;
  if (buffer_ && capacity_ > 0) buffer_[0] = '\0';
}

AcceptResult TextStreamReceiver::accept(const uint8_t* packet, const size_t length) {
  lastDiscardedBytes_ = 0;
  if (!packet || !buffer_ || capacity_ == 0 || length < PACKET_HEADER_BYTES || length > MAX_PACKET_BYTES) {
    return AcceptResult::INVALID_PACKET;
  }
  if (packet[0] != MAGIC_0 || packet[1] != MAGIC_1) return AcceptResult::INVALID_PACKET;
  if (packet[2] != PROTOCOL_VERSION) return AcceptResult::UNSUPPORTED_VERSION;

  const auto type = static_cast<PacketType>(packet[3]);
  const uint32_t sequence = readLe32(packet + 4);
  const size_t payloadLength = readLe16(packet + 8);
  if (payloadLength != length - PACKET_HEADER_BYTES) return AcceptResult::INVALID_PACKET;

  switch (type) {
    case PacketType::STREAM_RESET:
      return acceptReset(sequence, payloadLength);
    case PacketType::STREAM_APPEND:
      return acceptAppend(sequence, packet + PACKET_HEADER_BYTES, payloadLength);
    case PacketType::ACTION:
    case PacketType::COMMAND:
    default:
      return AcceptResult::UNEXPECTED_TYPE;
  }
}

AcceptResult TextStreamReceiver::acceptReset(const uint32_t sequence, const size_t payloadLength) {
  if (payloadLength != 0) return AcceptResult::INVALID_PACKET;

  currentLength_ = 0;
  buffer_[0] = '\0';
  expectedSequence_ = sequence + 1U;
  synchronized_ = true;
  return AcceptResult::STREAM_RESET;
}

AcceptResult TextStreamReceiver::acceptAppend(const uint32_t sequence, const uint8_t* payload,
                                              const size_t payloadLength) {
  if (!synchronized_) return AcceptResult::NEEDS_RESET;
  if (sequence == expectedSequence_ - 1U) return AcceptResult::DUPLICATE_IGNORED;
  if (sequence != expectedSequence_) {
    synchronized_ = false;
    return AcceptResult::OUT_OF_ORDER;
  }
  if (payloadLength == 0) return AcceptResult::INVALID_PACKET;
  if (payloadLength > maxTextBytes_) {
    synchronized_ = false;
    return AcceptResult::TOO_LARGE;
  }
  if (!isValidDisplayText(payload, payloadLength)) {
    synchronized_ = false;
    return AcceptResult::INVALID_TEXT;
  }

  const size_t combinedLength = currentLength_ + payloadLength;
  if (combinedLength > maxTextBytes_) discardOldest(combinedLength - maxTextBytes_);

  std::memcpy(buffer_ + currentLength_, payload, payloadLength);
  currentLength_ += payloadLength;
  buffer_[currentLength_] = '\0';
  expectedSequence_++;
  return AcceptResult::TEXT_APPENDED;
}

void TextStreamReceiver::discardOldest(const size_t bytesNeeded) {
  size_t discard = bytesNeeded;

  // Prefer dropping whole lines. If the current buffer has no later newline,
  // round forward to a UTF-8 code-point boundary instead.
  while (discard < currentLength_ && buffer_[discard - 1] != '\n') ++discard;
  if (discard < currentLength_ && buffer_[discard - 1] == '\n') {
    // The required prefix already ended exactly after a newline.
  } else if (discard < currentLength_ && buffer_[discard] == '\n') {
    ++discard;
  } else {
    discard = bytesNeeded;
    while (discard < currentLength_ && isUtf8Continuation(buffer_[discard])) ++discard;
  }

  std::memmove(buffer_, buffer_ + discard, currentLength_ - discard);
  currentLength_ -= discard;
  lastDiscardedBytes_ = discard;
}

}  // namespace ble_terminal
