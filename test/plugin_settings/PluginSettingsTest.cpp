#include <gtest/gtest.h>
#include <nvs.h>

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "src/plugins/PluginAbi.h"

namespace {

std::map<std::string, std::vector<uint8_t>> blobs;
int opens = 0;
int closes = 0;
int writes = 0;
int commits = 0;
esp_err_t openError = ESP_OK;
esp_err_t writeError = ESP_OK;
esp_err_t commitError = ESP_OK;

class PluginSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    blobs.clear();
    opens = closes = writes = commits = 0;
    openError = writeError = commitError = ESP_OK;
  }
  void TearDown() override { EXPECT_EQ(opens, closes); }
};

}  // namespace

esp_err_t nvs_open(const char* name, nvs_open_mode_t, nvs_handle_t* handle) {
  EXPECT_STREQ(name, "plugins");
  if (openError != ESP_OK) return openError;
  ++opens;
  *handle = 1;
  return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
  EXPECT_EQ(handle, 1U);
  ++closes;
}

esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* data, size_t* length) {
  const auto found = blobs.find(key);
  if (found == blobs.end()) return ESP_ERR_NVS_NOT_FOUND;
  const size_t capacity = *length;
  *length = found->second.size();
  if (capacity < *length) return ESP_ERR_NVS_INVALID_LENGTH;
  std::memcpy(data, found->second.data(), *length);
  return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* data, size_t length) {
  if (writeError != ESP_OK) return writeError;
  const auto* bytes = static_cast<const uint8_t*>(data);
  blobs[key] = std::vector<uint8_t>(bytes, bytes + length);
  ++writes;
  return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t) {
  ++commits;
  return commitError;
}

TEST_F(PluginSettingsTest, MissingSettingsDoNotWriteOrCreateDefaults) {
  uint8_t value = 42;
  size_t length = 999;
  EXPECT_FALSE(crosspoint_plugin_settings_read_v5("sample", &value, sizeof(value), &length));
  EXPECT_EQ(length, 0U);
  EXPECT_EQ(value, 42);
  EXPECT_EQ(writes, 0);
  EXPECT_EQ(commits, 0);
}

TEST_F(PluginSettingsTest, IndependentKeysAndUnchangedWrites) {
  const uint8_t first = 3;
  const uint8_t second = 7;
  EXPECT_TRUE(crosspoint_plugin_settings_write_v5("sample", &first, 1));
  EXPECT_TRUE(crosspoint_plugin_settings_write_v5("sample", &first, 1));
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(commits, 1);
  EXPECT_TRUE(crosspoint_plugin_settings_write_v5("other", &second, 1));
  uint8_t value = 0;
  size_t length = 0;
  EXPECT_TRUE(crosspoint_plugin_settings_read_v5("sample", &value, 1, &length));
  EXPECT_EQ(value, first);
  EXPECT_EQ(length, 1U);
  EXPECT_TRUE(crosspoint_plugin_settings_read_v5("other", &value, 1, &length));
  EXPECT_EQ(value, second);
  EXPECT_TRUE(crosspoint_plugin_settings_write_v5("sample", &second, 1));
  EXPECT_EQ(writes, 3);
}

TEST_F(PluginSettingsTest, InvalidKeysAndSizesNeverReachNvs) {
  std::array<uint8_t, crosspoint_plugin::SETTINGS_MAX_BYTES + 1> data{};
  size_t length = 0;
  for (const char* key : {"", "../ota", "Bad", "0123456789abcdef"}) {
    EXPECT_FALSE(crosspoint_plugin_settings_write_v5(key, data.data(), 1));
    EXPECT_FALSE(crosspoint_plugin_settings_read_v5(key, data.data(), 1, &length));
  }
  EXPECT_FALSE(crosspoint_plugin_settings_write_v5(nullptr, data.data(), 1));
  EXPECT_FALSE(crosspoint_plugin_settings_write_v5("sample", nullptr, 1));
  EXPECT_FALSE(crosspoint_plugin_settings_write_v5("sample", data.data(), 0));
  EXPECT_FALSE(crosspoint_plugin_settings_write_v5("sample", data.data(), data.size()));
  EXPECT_EQ(opens, 0);
}

TEST_F(PluginSettingsTest, MaximumSizeAndShortReadBuffer) {
  std::array<uint8_t, crosspoint_plugin::SETTINGS_MAX_BYTES> data{};
  EXPECT_TRUE(crosspoint_plugin_settings_write_v5("0123456789abcde", data.data(), data.size()));
  size_t length = 999;
  EXPECT_FALSE(crosspoint_plugin_settings_read_v5("0123456789abcde", data.data(), 1, &length));
  EXPECT_EQ(length, 0U);
  EXPECT_TRUE(crosspoint_plugin_settings_read_v5("0123456789abcde", data.data(), data.size(), &length));
  EXPECT_EQ(length, data.size());
}

TEST_F(PluginSettingsTest, StorageErrorsAreReturnedWithoutErasingAnything) {
  const uint8_t value = 9;
  openError = ESP_FAIL;
  EXPECT_FALSE(crosspoint_plugin_settings_write_v5("sample", &value, 1));
  EXPECT_EQ(opens, 0);
  openError = ESP_OK;
  writeError = ESP_FAIL;
  EXPECT_FALSE(crosspoint_plugin_settings_write_v5("sample", &value, 1));
  EXPECT_EQ(commits, 0);
  writeError = ESP_OK;
  commitError = ESP_FAIL;
  EXPECT_FALSE(crosspoint_plugin_settings_write_v5("sample", &value, 1));
  EXPECT_EQ(commits, 1);
}
