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

std::vector<uint8_t> packet(const PacketType type, const uint32_t sequence, const std::string_view payload = {}) {
  std::vector<uint8_t> out{'X', 'T', 1, static_cast<uint8_t>(type)};
  appendLe32(out, sequence);
  appendLe16(out, static_cast<uint16_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

template <size_t Capacity = 128>
struct ReceiverFixture {
  std::array<char, Capacity> storage{};
  ble_terminal::TextStreamReceiver receiver{storage.data(), Capacity};

  AcceptResult accept(const std::vector<uint8_t>& value) { return receiver.accept(value.data(), value.size()); }
  AcceptResult reset(const uint32_t sequence) { return accept(packet(PacketType::STREAM_RESET, sequence)); }
  AcceptResult append(const uint32_t sequence, const std::string_view text) {
    return accept(packet(PacketType::STREAM_APPEND, sequence, text));
  }
};

TEST(BleTerminalProtocol, AppendsNewTextAsPacketsArrive) {
  ReceiverFixture fixture;

  EXPECT_EQ(fixture.reset(40), AcceptResult::STREAM_RESET);
  EXPECT_EQ(fixture.append(41, "user\n"), AcceptResult::TEXT_APPENDED);
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "user\n");
  EXPECT_EQ(fixture.append(42, "hello terminal\n"), AcceptResult::TEXT_APPENDED);
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "user\nhello terminal\n");
  EXPECT_EQ(fixture.receiver.expectedSequence(), 43U);
}

TEST(BleTerminalProtocol, AcceptsUtf8WhenEachPacketEndsOnACodepointBoundary) {
  ReceiverFixture fixture;

  ASSERT_EQ(fixture.reset(1), AcceptResult::STREAM_RESET);
  EXPECT_EQ(fixture.append(2, "Привет, "), AcceptResult::TEXT_APPENDED);
  EXPECT_EQ(fixture.append(3, "terminal"), AcceptResult::TEXT_APPENDED);
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "Привет, terminal");
}

TEST(BleTerminalProtocol, RequiresResetBeforeFirstAppend) {
  ReceiverFixture fixture;
  EXPECT_EQ(fixture.append(1, "text"), AcceptResult::NEEDS_RESET);
  EXPECT_STREQ(fixture.receiver.currentText(), "");
}

TEST(BleTerminalProtocol, GapPreservesTextAndRequiresReset) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.reset(10), AcceptResult::STREAM_RESET);
  ASSERT_EQ(fixture.append(11, "known good"), AcceptResult::TEXT_APPENDED);

  EXPECT_EQ(fixture.append(13, "missed packet 12"), AcceptResult::OUT_OF_ORDER);
  EXPECT_FALSE(fixture.receiver.synchronized());
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "known good");
  EXPECT_EQ(fixture.append(14, "still blocked"), AcceptResult::NEEDS_RESET);
}

TEST(BleTerminalProtocol, ResetWithAnySequenceResynchronizesAndClearsText) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.reset(10), AcceptResult::STREAM_RESET);
  ASSERT_EQ(fixture.append(11, "old"), AcceptResult::TEXT_APPENDED);
  ASSERT_EQ(fixture.append(13, "gap"), AcceptResult::OUT_OF_ORDER);

  EXPECT_EQ(fixture.reset(500), AcceptResult::STREAM_RESET);
  EXPECT_TRUE(fixture.receiver.synchronized());
  EXPECT_STREQ(fixture.receiver.currentText(), "");
  EXPECT_EQ(fixture.append(501, "new"), AcceptResult::TEXT_APPENDED);
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "new");
}

TEST(BleTerminalProtocol, DuplicatePacketIsIgnored) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.reset(7), AcceptResult::STREAM_RESET);
  ASSERT_EQ(fixture.append(8, "once"), AcceptResult::TEXT_APPENDED);

  EXPECT_EQ(fixture.append(8, "once"), AcceptResult::DUPLICATE_IGNORED);
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "once");
  EXPECT_TRUE(fixture.receiver.synchronized());
}

TEST(BleTerminalProtocol, RejectsInvalidUtf8AndAnsiControls) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.reset(1), AcceptResult::STREAM_RESET);

  const std::string invalidUtf8{"\xF0\x28\x8C\x28", 4};
  EXPECT_EQ(fixture.append(2, invalidUtf8), AcceptResult::INVALID_TEXT);
  EXPECT_FALSE(fixture.receiver.synchronized());

  ASSERT_EQ(fixture.reset(10), AcceptResult::STREAM_RESET);
  EXPECT_EQ(fixture.append(11, "hello\x1B[31m"), AcceptResult::INVALID_TEXT);
  EXPECT_FALSE(fixture.receiver.synchronized());
}

TEST(BleTerminalProtocol, DiscardsOldestWholeLinesAtCapacity) {
  ReceiverFixture<16> fixture;
  ASSERT_EQ(fixture.reset(1), AcceptResult::STREAM_RESET);
  ASSERT_EQ(fixture.append(2, "one\ntwo\n"), AcceptResult::TEXT_APPENDED);
  ASSERT_EQ(fixture.append(3, "three\n"), AcceptResult::TEXT_APPENDED);
  ASSERT_EQ(fixture.append(4, "four\n"), AcceptResult::TEXT_APPENDED);

  EXPECT_EQ(fixture.receiver.currentLength(), 15U);
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "two\nthree\nfour\n");
  EXPECT_EQ(fixture.receiver.lastDiscardedBytes(), 4U);

  ASSERT_EQ(fixture.append(5, "five\n"), AcceptResult::TEXT_APPENDED);
  EXPECT_EQ(fixture.receiver.lastDiscardedBytes(), 10U);
  EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "four\nfive\n");
}

TEST(BleTerminalProtocol, RejectsPacketLargerThanReceiverCapacity) {
  ReceiverFixture<8> fixture;
  ASSERT_EQ(fixture.reset(1), AcceptResult::STREAM_RESET);
  EXPECT_EQ(fixture.append(2, "12345678"), AcceptResult::TOO_LARGE);
  EXPECT_FALSE(fixture.receiver.synchronized());
}

TEST(BleTerminalProtocol, RejectsMalformedHeaderAndLength) {
  ReceiverFixture fixture;
  auto reset = packet(PacketType::STREAM_RESET, 1);

  reset[0] = 'B';
  EXPECT_EQ(fixture.accept(reset), AcceptResult::INVALID_PACKET);
  reset[0] = 'X';
  reset[2] = 2;
  EXPECT_EQ(fixture.accept(reset), AcceptResult::UNSUPPORTED_VERSION);
  reset[2] = 1;
  reset[8]++;
  EXPECT_EQ(fixture.accept(reset), AcceptResult::INVALID_PACKET);
}

TEST(BleTerminalProtocol, TruncatedPacketsCannotChangeText) {
  ReceiverFixture fixture;
  ASSERT_EQ(fixture.reset(1), AcceptResult::STREAM_RESET);
  ASSERT_EQ(fixture.append(2, "stable"), AcceptResult::TEXT_APPENDED);
  const auto valid = packet(PacketType::STREAM_APPEND, 3, "new text");

  for (size_t length = 0; length < valid.size(); ++length) {
    EXPECT_NE(fixture.receiver.accept(valid.data(), length), AcceptResult::TEXT_APPENDED);
    EXPECT_EQ(std::string_view(fixture.receiver.currentText()), "stable");
  }
}

TEST(BleTerminalProtocol, EncodesOnlyAllowListedActions) {
  std::array<uint8_t, ble_terminal::PACKET_HEADER_BYTES + 1> output{};
  const size_t length =
      ble_terminal::encodeActionPacket(ble_terminal::Action::INTERRUPT_SESSION, 0x12345678U, output.data(), output.size());

  ASSERT_EQ(length, output.size());
  const std::array<uint8_t, ble_terminal::PACKET_HEADER_BYTES + 1> expected{
      'X', 'T', 1, static_cast<uint8_t>(PacketType::ACTION), 0x78, 0x56, 0x34, 0x12, 1, 0,
      static_cast<uint8_t>(ble_terminal::Action::INTERRUPT_SESSION)};
  EXPECT_EQ(output, expected);
  EXPECT_EQ(ble_terminal::encodeActionPacket(static_cast<ble_terminal::Action>(0xFF), 1, output.data(), output.size()),
            0U);
  EXPECT_EQ(
      ble_terminal::encodeActionPacket(ble_terminal::Action::SUBMIT_INPUT, 1, output.data(), output.size() - 1), 0U);

  EXPECT_EQ(ble_terminal::encodeActionPacket(ble_terminal::Action::PAGE_UP, 9, output.data(), output.size()),
            output.size());
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES], static_cast<uint8_t>(ble_terminal::Action::PAGE_UP));
  EXPECT_EQ(ble_terminal::encodeActionPacket(ble_terminal::Action::PAGE_DOWN, 10, output.data(), output.size()),
            output.size());
  EXPECT_EQ(output[ble_terminal::PACKET_HEADER_BYTES], static_cast<uint8_t>(ble_terminal::Action::PAGE_DOWN));
}

TEST(BleTerminalProtocol, EncodesOneUtf8CommandLine) {
  std::array<uint8_t, ble_terminal::MAX_PACKET_BYTES> output{};
  const std::string command = "echo Привет";
  const size_t length = ble_terminal::encodeCommandPacket(
      reinterpret_cast<const uint8_t*>(command.data()), command.size(), 0x12345678U, output.data(), output.size());

  ASSERT_EQ(length, ble_terminal::PACKET_HEADER_BYTES + command.size());
  EXPECT_EQ(output[0], 'X');
  EXPECT_EQ(output[1], 'T');
  EXPECT_EQ(output[2], 1);
  EXPECT_EQ(output[3], static_cast<uint8_t>(PacketType::COMMAND));
  EXPECT_EQ(output[4], 0x78);
  EXPECT_EQ(output[5], 0x56);
  EXPECT_EQ(output[6], 0x34);
  EXPECT_EQ(output[7], 0x12);
  EXPECT_EQ(output[8], command.size());
  EXPECT_EQ(output[9], 0);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(output.data() + ble_terminal::PACKET_HEADER_BYTES),
                             command.size()),
            command);
}

TEST(BleTerminalProtocol, RejectsInvalidCommandLines) {
  std::array<uint8_t, ble_terminal::MAX_PACKET_BYTES> output{};
  const auto encode = [&](const std::string& command) {
    return ble_terminal::encodeCommandPacket(reinterpret_cast<const uint8_t*>(command.data()), command.size(), 1,
                                             output.data(), output.size());
  };

  EXPECT_EQ(encode(""), 0U);
  EXPECT_EQ(encode("two\nlines"), 0U);
  EXPECT_EQ(encode("tab\tcompletion"), 0U);
  EXPECT_EQ(encode(std::string("bad\x1b" "command", 11)), 0U);
  EXPECT_EQ(encode(std::string(ble_terminal::MAX_COMMAND_BYTES + 1, 'x')), 0U);

  const std::string maximum(ble_terminal::MAX_COMMAND_BYTES, 'x');
  EXPECT_EQ(encode(maximum), ble_terminal::MAX_PACKET_BYTES);
  EXPECT_EQ(ble_terminal::encodeCommandPacket(reinterpret_cast<const uint8_t*>(maximum.data()), maximum.size(), 1,
                                              output.data(), output.size() - 1),
            0U);
}

}  // namespace
