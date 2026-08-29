#pragma once

#include <cstddef>
#include <cstdint>

namespace ble_terminal {

// Application packets are sized for a negotiated ATT MTU of 247
// (244 characteristic-value bytes). Smaller MTUs use smaller APPEND packets.
constexpr size_t PACKET_HEADER_BYTES = 10;
constexpr size_t MAX_PACKET_BYTES = 244;
constexpr size_t MAX_COMMAND_BYTES = MAX_PACKET_BYTES - PACKET_HEADER_BYTES;
constexpr size_t MAX_TRANSCRIPT_BYTES = 32 * 1024;
static_assert(MAX_TRANSCRIPT_BYTES <= 0xFFFFU);

enum class PacketType : uint8_t {
  STREAM_RESET = 1,
  STREAM_APPEND = 2,
  ACTION = 3,
  COMMAND = 4,
  STREAM_TRUNCATE = 5,
};

// Deliberately small allow-list for non-text controls.
enum class Action : uint8_t {
  INTERRUPT_SESSION = 1,
  APPROVE_REQUEST = 2,
  REJECT_REQUEST = 3,
  SUBMIT_INPUT = 4,
  PAGE_UP = 5,
  PAGE_DOWN = 6,
};

enum class AcceptResult : uint8_t {
  STREAM_RESET,
  TEXT_APPENDED,
  TEXT_TRUNCATED,
  DUPLICATE_IGNORED,
  INVALID_PACKET,
  UNSUPPORTED_VERSION,
  UNEXPECTED_TYPE,
  NEEDS_RESET,
  OUT_OF_ORDER,
  TOO_LARGE,
  INVALID_TEXT,
  INVALID_TRUNCATE,
};

bool isValidDisplayText(const uint8_t* data, size_t length);
bool isValidCommandText(const uint8_t* data, size_t length);
size_t encodeActionPacket(Action action, uint32_t sequence, uint8_t* output, size_t capacity);
size_t encodeCommandPacket(const uint8_t* command, size_t commandLength, uint32_t sequence, uint8_t* output,
                           size_t capacity);

// Allocation-free receiver for untrusted BLE writes. Android first sends a
// STREAM_RESET packet and then sends plain UTF-8 in ordered STREAM_APPEND
// packets as output arrives. STREAM_TRUNCATE replaces a changed tail without
// discarding older pages. The buffer retains only the newest text; old complete
// lines are discarded first when space is needed.
class TextStreamReceiver final {
 public:
  TextStreamReceiver(char* buffer, size_t capacity);

  AcceptResult accept(const uint8_t* packet, size_t length);
  void clear();

  const char* currentText() const { return buffer_ ? buffer_ : ""; }
  size_t currentLength() const { return currentLength_; }
  size_t lastDiscardedBytes() const { return lastDiscardedBytes_; }
  uint32_t expectedSequence() const { return expectedSequence_; }
  bool synchronized() const { return synchronized_; }

 private:
  char* buffer_;
  size_t capacity_;
  size_t maxTextBytes_;
  size_t currentLength_ = 0;
  size_t lastDiscardedBytes_ = 0;
  uint32_t expectedSequence_ = 0;
  bool synchronized_ = false;

  AcceptResult acceptReset(uint32_t sequence, size_t payloadLength);
  AcceptResult acceptAppend(uint32_t sequence, const uint8_t* payload, size_t payloadLength);
  AcceptResult acceptTruncate(uint32_t sequence, const uint8_t* payload, size_t payloadLength);
  void discardOldest(size_t bytesNeeded);
};

}  // namespace ble_terminal
