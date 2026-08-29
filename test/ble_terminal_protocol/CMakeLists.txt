add_executable(BleTerminalProtocolTest
  BleTerminalProtocolTest.cpp
  ${REPO_ROOT}/src/ble/BleTerminalProtocol.cpp
)

target_include_directories(BleTerminalProtocolTest PRIVATE
  ${REPO_ROOT}/src/ble
)

target_link_libraries(BleTerminalProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleTerminalProtocolTest)
