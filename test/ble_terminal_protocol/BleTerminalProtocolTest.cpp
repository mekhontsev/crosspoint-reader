#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "BleTerminalProtocol.h"

namespace {

using ble_terminal::AcceptResult;
using ble_terminal::PacketType;

void appendLe16(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendLe32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8U));
  out.push_back(static_cast<uint8_t>(value >> 16U));
  out.push_back(static_cast<uint8_t>(value >> 24U));
}

uint32_t crc32(const std::string_view text) {
  uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char value : text) {
    crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xFFFFFFFFU;
}

std::vector<uint8_t> packet(const PacketType type, const uint32_t sequence, const std::string_view payload = {}) {
  std::vector<uint8_t> out{'X', 'T', 4, static_cast<uint8_t>(type)};
  appendLe32(out, sequence);
  appendLe16(out, static_cast<uint16_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

template <size_t Capacity = 128>
struct ReceiverFixture {
  std::array<char, Capacity> storage{};
  ble_terminal::TextFrameReceiver receiver{storage.data(), Capacity};

  AcceptResult accept(const std::vector<uint8_t>& value) { return receiver.accept(value.data(), value.size()); }

  AcceptResult begin(const uint32_t sequence, const uint32_t frameId, const std::string_view completeText,
                     const uint8_t flags = 0, const uint32_t overrideCrc = 0) {
    std::string payload;
    const auto append32 = [&](const uint32_t value) {
      payload.push_back(static_cast<char>(value));
      payload.push_back(static_cast<char>(value >> 8U));
      payload.push_back(static_cast<char>(value >> 16U));
      payload.push_back(static_cast<char>(value >> 24U));
    };
    append32(frameId);
    payload.push_back(static_cast<char>(completeText.size()));
    payload.push_back(static_cast<char>(completeText.size() >> 8U));
    append32(overrideCrc == 0 ? crc32(completeText) : overrideCrc);
    payload.push_back(static_cast<char>(flags));
    return accept(packet(PacketType::FRAME_BEGIN, sequence, payload));
  }

  AcceptResult data(const uint32_t sequence, const std::string_view text) {
    return accept(packet(PacketType::FRAME_DATA, sequence, text));
  }

  AcceptResult commit(const uint32_t sequence, const uint32_t frameId) {
    std::string payload;
    payload.push_back(static_cast<char>(frameId));
    payload.push_back(static_cast<char>(frameId >> 8U));
    payload.push_back(static_cast<char>(frameId >> 16U));
    payload.push_back(static_cast<char>(frameId >> 24U));
    return accept(packet(PacketType::FRAME_COMMIT, sequence, payload));
  }
};

TEST(BleTerminalProtocol, CommitsOnlyACompleteCrcCheckedFrame) {
  ReceiverFixture fixture;
  const std::string frame = "user\nПривет";

  EXPECT_EQ(fixture.begin(40, 7, frame, ble_terminal::FRAME_FLAG_LATEST), AcceptResult::FRAME_STARTED);
  EXPECT_EQ(fixture.data(41, "user\n"), AcceptResult::FRAME_DATA_ACCEPTED);
  EXPECT_TRUE(fixture.receiver.receiving());
  EXPECT_EQ(fixture.data(42, "Привет"), AcceptResult::FRAME_DATA_ACCEPTED);
  EXPECT_EQ(fixture.commit(43, 7), AcceptResult::FRAME_COMMITTED);
  EXPECT_FALSE(fixture.receiver.receiving());
  EXPECT_EQ(fixture.receiver.frameId(), 7U);
  EXPECT_EQ(fixture.receiver.frameFlags(), ble_terminal::FRAME_FLAG_LATEST);
  EXPECT_EQ(std::string_view(fixture.receiver.frameText()), frame);
}

TEST(BleTerminalProtocol, RejectsMissingDataAndCrcMismatch) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.begin(1, 9, "complete"), AcceptResult::FRAME_STARTED);
  ASSERT_EQ(fixture.data(2, "short"), AcceptResult::FRAME_DATA_ACCEPTED);
  EXPECT_EQ(fixture.commit(3, 9), AcceptResult::INVALID_FRAME);

  ASSERT_EQ(fixture.begin(10, 10, "complete", 0, 1), AcceptResult::FRAME_STARTED);
  ASSERT_EQ(fixture.data(11, "complete"), AcceptResult::FRAME_DATA_ACCEPTED);
  EXPECT_EQ(fixture.commit(12, 10), AcceptResult::CRC_MISMATCH);
}

TEST(BleTerminalProtocol, NewBeginResynchronizesAfterAnOrderingGap) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.begin(10, 1, "old"), AcceptResult::FRAME_STARTED);
  EXPECT_EQ(fixture.data(12, "old"), AcceptResult::OUT_OF_ORDER);
  EXPECT_FALSE(fixture.receiver.receiving());
  EXPECT_EQ(fixture.data(13, "blocked"), AcceptResult::NEEDS_BEGIN);

  ASSERT_EQ(fixture.begin(500, 2, "new"), AcceptResult::FRAME_STARTED);
  ASSERT_EQ(fixture.data(501, "new"), AcceptResult::FRAME_DATA_ACCEPTED);
  EXPECT_EQ(fixture.commit(502, 2), AcceptResult::FRAME_COMMITTED);
  EXPECT_STREQ(fixture.receiver.frameText(), "new");
}

TEST(BleTerminalProtocol, DuplicateDataAndCommitAreIgnored) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.begin(7, 5, "once"), AcceptResult::FRAME_STARTED);
  ASSERT_EQ(fixture.data(8, "once"), AcceptResult::FRAME_DATA_ACCEPTED);
  EXPECT_EQ(fixture.data(8, "once"), AcceptResult::DUPLICATE_IGNORED);
  ASSERT_EQ(fixture.commit(9, 5), AcceptResult::FRAME_COMMITTED);
  EXPECT_EQ(fixture.commit(9, 5), AcceptResult::DUPLICATE_IGNORED);
}

TEST(BleTerminalProtocol, CommitsAnEmptyFrameWithoutDataPackets) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.begin(20, 6, "", ble_terminal::FRAME_FLAG_PRESENT), AcceptResult::FRAME_STARTED);
  EXPECT_EQ(fixture.commit(21, 6), AcceptResult::FRAME_COMMITTED);
  EXPECT_EQ(fixture.receiver.frameLength(), 0U);
  EXPECT_STREQ(fixture.receiver.frameText(), "");
}

TEST(BleTerminalProtocol, RejectsInvalidUtf8AndOversizedFrame) {
  ReceiverFixture<16> fixture;
  const std::string invalidUtf8{"\xF0\x28\x8C\x28", 4};
  ASSERT_EQ(fixture.begin(1, 1, invalidUtf8), AcceptResult::FRAME_STARTED);
  EXPECT_EQ(fixture.data(2, invalidUtf8), AcceptResult::INVALID_TEXT);

  EXPECT_EQ(fixture.begin(10, 2, std::string(16, 'x')), AcceptResult::TOO_LARGE);
}

TEST(BleTerminalProtocol, RejectsMalformedHeaderAndVersion) {
  ReceiverFixture fixture;
  auto begin = packet(PacketType::FRAME_BEGIN, 1, std::string(11, '\0'));
  begin[0] = 'B';
  EXPECT_EQ(fixture.accept(begin), AcceptResult::INVALID_PACKET);
  begin[0] = 'X';
  begin[2] = 2;
  EXPECT_EQ(fixture.accept(begin), AcceptResult::UNSUPPORTED_VERSION);
  begin[2] = 4;
  begin[8]++;
  EXPECT_EQ(fixture.accept(begin), AcceptResult::INVALID_PACKET);
}

TEST(BleTerminalProtocol, RejectsUnknownFrameFlags) {
  ReceiverFixture fixture;
  EXPECT_EQ(fixture.begin(1, 1, "text", 0x80), AcceptResult::INVALID_FRAME);
  EXPECT_FALSE(fixture.receiver.receiving());
}

TEST(BleTerminalProtocol, EncodesReaderControlsAndCommands) {
  std::array<uint8_t, ble_terminal::MAX_PACKET_BYTES> output{};
  size_t length = ble_terminal::encodeFrameRequestPacket(ble_terminal::FrameRequest::PREVIOUS, 0x12345678U, 7,
                                                         output.data(), output.size());
  ASSERT_EQ(length, ble_terminal::PACKET_HEADER_BYTES + 5);
  EXPECT_EQ(output[2], 4);
  EXPECT_EQ(output[3], static_cast<uint8_t>(PacketType::FRAME_REQUEST));
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES], static_cast<uint8_t>(ble_terminal::FrameRequest::PREVIOUS));
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES + 1], 0x78);

  length = ble_terminal::encodeFrameStatusPacket(42, ble_terminal::FrameStatus::READY, 8, output.data(), output.size());
  ASSERT_EQ(length, ble_terminal::PACKET_HEADER_BYTES + 5);
  EXPECT_EQ(output[3], static_cast<uint8_t>(PacketType::FRAME_STATUS));
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES], 42);

  length = ble_terminal::encodeViewportPacket(42, 38, 9, output.data(), output.size());
  ASSERT_EQ(length, ble_terminal::PACKET_HEADER_BYTES + 4);
  EXPECT_EQ(output[3], static_cast<uint8_t>(PacketType::VIEWPORT));
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES], 42);
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES + 2], 38);
  EXPECT_EQ(ble_terminal::encodeViewportPacket(0, 38, 9, output.data(), output.size()), 0U);

  const std::string command = "echo Привет";
  length = ble_terminal::encodeCommandPacket(reinterpret_cast<const uint8_t*>(command.data()), command.size(), 9,
                                             output.data(), output.size());
  ASSERT_EQ(length, ble_terminal::PACKET_HEADER_BYTES + command.size());
  EXPECT_EQ(output[3], static_cast<uint8_t>(PacketType::COMMAND));
  EXPECT_EQ(ble_terminal::encodeCommandPacket(reinterpret_cast<const uint8_t*>("two\nlines"), 9, 10, output.data(),
                                              output.size()),
            0U);
}

TEST(BleTerminalProtocol, EncodesPluginUpdateCapabilitiesAndStatus) {
  std::array<uint8_t, ble_terminal::MAX_PACKET_BYTES> output{};
  size_t length = ble_terminal::encodePluginUpdateHelloPacket(3, ble_terminal::MAX_UPDATE_DATA_BYTES, 11, output.data(),
                                                              output.size());
  ASSERT_EQ(length, ble_terminal::PACKET_HEADER_BYTES + 6);
  EXPECT_EQ(output[3], static_cast<uint8_t>(PacketType::PLUGIN_UPDATE_HELLO));
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES], 3);
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES + 4], ble_terminal::MAX_UPDATE_DATA_BYTES & 0xffU);

  ble_terminal::PacketView view{};
  ASSERT_TRUE(ble_terminal::decodePacket(output.data(), length, view));
  EXPECT_EQ(view.type, PacketType::PLUGIN_UPDATE_HELLO);
  EXPECT_EQ(view.sequence, 11U);
  EXPECT_EQ(view.payloadLength, 6U);

  length = ble_terminal::encodePluginUpdateStatusPacket(2, 300972, 12, output.data(), output.size());
  ASSERT_EQ(length, ble_terminal::PACKET_HEADER_BYTES + 5);
  EXPECT_EQ(output[3], static_cast<uint8_t>(PacketType::PLUGIN_UPDATE_STATUS));
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES], 2);
  EXPECT_EQ(ble_terminal::encodePluginUpdateHelloPacket(3, 0, 13, output.data(), output.size()), 0U);
  EXPECT_EQ(ble_terminal::encodePluginUpdateStatusPacket(0, 0, 13, output.data(), output.size()), 0U);
}

}  // namespace
