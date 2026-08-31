#pragma once

#include <cstddef>
#include <cstdint>

namespace ble_terminal {

constexpr size_t PACKET_HEADER_BYTES = 10;
constexpr size_t MAX_PACKET_BYTES = 244;
constexpr size_t MAX_COMMAND_BYTES = MAX_PACKET_BYTES - PACKET_HEADER_BYTES;
constexpr size_t MAX_FRAME_BYTES = 6 * 1024;

enum class PacketType : uint8_t {
  FRAME_BEGIN = 1,
  FRAME_DATA = 2,
  ACTION = 3,
  COMMAND = 4,
  FRAME_COMMIT = 5,
  FRAME_REQUEST = 6,
  FRAME_STATUS = 7,
  VIEWPORT = 8,
};

enum class Action : uint8_t {
  INTERRUPT_SESSION = 1,
  APPROVE_REQUEST = 2,
  REJECT_REQUEST = 3,
  SUBMIT_INPUT = 4,
};

enum class FrameRequest : uint8_t { CURRENT = 0, PREVIOUS = 1, NEXT = 2 };
enum class FrameStatus : uint8_t { READY = 0, RETRY = 1 };

constexpr uint8_t FRAME_FLAG_LATEST = 1U;
constexpr uint8_t FRAME_FLAG_PRESENT = 1U << 1U;
constexpr uint8_t FRAME_FLAG_RESET_CACHE = 1U << 2U;
constexpr uint8_t FRAME_FLAGS_MASK = FRAME_FLAG_LATEST | FRAME_FLAG_PRESENT | FRAME_FLAG_RESET_CACHE;

enum class AcceptResult : uint8_t {
  FRAME_STARTED,
  FRAME_DATA_ACCEPTED,
  FRAME_COMMITTED,
  DUPLICATE_IGNORED,
  INVALID_PACKET,
  UNSUPPORTED_VERSION,
  UNEXPECTED_TYPE,
  NEEDS_BEGIN,
  OUT_OF_ORDER,
  TOO_LARGE,
  INVALID_TEXT,
  INVALID_FRAME,
  CRC_MISMATCH,
};

bool isValidDisplayText(const uint8_t* data, size_t length);
bool isValidCommandText(const uint8_t* data, size_t length);
size_t encodeActionPacket(Action action, uint32_t sequence, uint8_t* output, size_t capacity);
size_t encodeCommandPacket(const uint8_t* command, size_t commandLength, uint32_t sequence, uint8_t* output,
                           size_t capacity);
size_t encodeFrameRequestPacket(FrameRequest request, uint32_t anchorFrameId, uint32_t sequence, uint8_t* output,
                                size_t capacity);
size_t encodeFrameStatusPacket(uint32_t frameId, FrameStatus status, uint32_t sequence, uint8_t* output,
                               size_t capacity);
size_t encodeViewportPacket(uint16_t columns, uint16_t rows, uint32_t sequence, uint8_t* output, size_t capacity);

class TextFrameReceiver final {
 public:
  TextFrameReceiver(char* buffer, size_t capacity);

  AcceptResult accept(const uint8_t* packet, size_t length);
  void clear();

  const char* frameText() const { return buffer_ ? buffer_ : ""; }
  size_t frameLength() const { return currentLength_; }
  uint32_t frameId() const { return frameId_; }
  uint8_t frameFlags() const { return frameFlags_; }
  bool receiving() const { return receiving_; }

 private:
  char* buffer_;
  size_t capacity_;
  size_t currentLength_ = 0;
  size_t expectedLength_ = 0;
  uint32_t expectedCrc_ = 0;
  uint32_t frameId_ = 0;
  uint8_t frameFlags_ = 0;
  uint32_t beginSequence_ = 0;
  uint32_t expectedSequence_ = 0;
  uint32_t committedSequence_ = 0;
  uint32_t committedFrameId_ = 0;
  bool receiving_ = false;
  bool hasCommittedFrame_ = false;

  AcceptResult acceptBegin(uint32_t sequence, const uint8_t* payload, size_t payloadLength);
  AcceptResult acceptData(uint32_t sequence, const uint8_t* payload, size_t payloadLength);
  AcceptResult acceptCommit(uint32_t sequence, const uint8_t* payload, size_t payloadLength);
};

}  // namespace ble_terminal
