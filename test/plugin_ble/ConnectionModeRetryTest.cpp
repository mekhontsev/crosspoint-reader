#include <gtest/gtest.h>

#include "src/ble/ConnectionModeRetry.h"

TEST(ConnectionModeRetry, IdleDoesNotRetryAndSuccessStopsAllChecks) {
  plugin_ble::ConnectionModeRetry retry;
  EXPECT_FALSE(retry.due(100));
  retry.reset(100);
  EXPECT_TRUE(retry.due(100));
  retry.finish();
  EXPECT_FALSE(retry.due(100000));
}

TEST(ConnectionModeRetry, RejectedRequestsAreBoundedAndBackOff) {
  plugin_ble::ConnectionModeRetry retry;
  retry.reset(100);
  retry.attempted(100);
  EXPECT_FALSE(retry.due(1099));
  EXPECT_TRUE(retry.due(1100));
  retry.attempted(1100);
  EXPECT_FALSE(retry.due(3099));
  EXPECT_TRUE(retry.due(3100));
  retry.attempted(3100);
  EXPECT_TRUE(retry.exhausted());
  EXPECT_FALSE(retry.due(7099));
  EXPECT_TRUE(retry.due(7100));
  retry.finish();
  EXPECT_FALSE(retry.due(100000));
  retry.reset(100001);
  EXPECT_FALSE(retry.exhausted());
  EXPECT_TRUE(retry.due(100001));
}

TEST(ConnectionModeRetry, DeadlineSurvivesMillisWrap) {
  plugin_ble::ConnectionModeRetry retry;
  retry.reset(UINT32_MAX - 10);
  retry.attempted(UINT32_MAX - 10);
  EXPECT_FALSE(retry.due(UINT32_MAX));
  EXPECT_FALSE(retry.due(988));
  EXPECT_TRUE(retry.due(989));
}
