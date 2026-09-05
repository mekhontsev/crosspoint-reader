#include <HalStorage.h>
#include <gtest/gtest.h>

#include "src/plugins/PluginAbi.h"

namespace {
size_t provider(void* context, const uint8_t* input, size_t, uint8_t* output, size_t) {
  ++*static_cast<int*>(context);
  output[0] = input[0];
  return 1;
}
}  // namespace

TEST(PluginServices, ProviderOwnershipAndLifetime) {
  int first = 0, second = 0;
  uint8_t input[] = {7}, output[8]{};
  ASSERT_TRUE(crosspoint_plugin_state_register_v5("one", provider, &first));
  ASSERT_FALSE(crosspoint_plugin_state_register_v5("one", provider, &second));
  ASSERT_TRUE(crosspoint_plugin_state_register_v5("one", provider, &first));
  EXPECT_EQ(crosspoint_plugin_state_call_v5("one", input, 1, output, 8), 1);
  EXPECT_EQ(first, 1);
  EXPECT_EQ(output[0], 7);
  crosspoint_plugin_state_unregister_v5("one", &second);
  EXPECT_EQ(crosspoint_plugin_state_call_v5("one", input, 1, output, 8), 1);
  crosspoint_plugin_state_unregister_v5("one", &first);
  EXPECT_EQ(crosspoint_plugin_state_call_v5("one", input, 1, output, 8), 0);
}

TEST(PluginServices, RejectInvalidAndUnboundedProviderCalls) {
  int context = 0;
  uint8_t packet[240]{};
  for (const auto* name : {"", "../one", "UPPER", "1234567890123456"}) {
    EXPECT_FALSE(crosspoint_plugin_state_register_v5(name, provider, &context));
  }
  ASSERT_TRUE(crosspoint_plugin_state_register_v5("one", provider, &context));
  EXPECT_EQ(crosspoint_plugin_state_call_v5("one", packet, 240, packet, 239), 0);
  EXPECT_EQ(crosspoint_plugin_state_call_v5("one", packet, 1, packet, 240), 0);
  EXPECT_EQ(crosspoint_plugin_state_call_v5("one", nullptr, 1, packet, 8), 0);
  EXPECT_EQ(context, 0);
  crosspoint_plugin_state_unregister_v5("one", &context);
}

TEST(PluginServices, BoundedReadOnlySdAndEof) {
  uint8_t data[224]{};
  size_t bytes = 99;
  uint32_t total = 99;
  ASSERT_TRUE(crosspoint_plugin_file_read_v5("/crash_report.txt", 2, data, 3, &bytes, &total));
  EXPECT_EQ(bytes, 3);
  EXPECT_EQ(total, 6);
  EXPECT_EQ(std::memcmp(data, "cde", 3), 0);
  EXPECT_EQ(Storage.lastMode, O_RDONLY);
  ASSERT_TRUE(crosspoint_plugin_file_read_v5("/file", 6, data, sizeof(data), &bytes, &total));
  EXPECT_EQ(bytes, 0);
  EXPECT_FALSE(crosspoint_plugin_file_read_v5("/missing", 0, data, sizeof(data), &bytes, &total));
  EXPECT_EQ(bytes, 0);
  EXPECT_FALSE(crosspoint_plugin_file_read_v5("/file", 7, data, sizeof(data), &bytes, &total));
}

TEST(PluginServices, RejectUnsafePathsAndOversizedReadsBeforeSdAccess) {
  uint8_t data[225]{};
  size_t bytes = 0;
  uint32_t total = 0;
  const int opens = Storage.opens;
  for (const char* path : {"relative", "/a/../b", "/./b", "/a/..", "/a\\b", "/a\nb"}) {
    EXPECT_FALSE(crosspoint_plugin_file_read_v5(path, 0, data, 224, &bytes, &total));
  }
  EXPECT_FALSE(crosspoint_plugin_file_read_v5("/file", 0, data, 225, &bytes, &total));
  EXPECT_EQ(Storage.opens, opens);
  EXPECT_TRUE(crosspoint_plugin_file_read_v5("/.hidden/file", 0, data, 224, &bytes, &total));
}
