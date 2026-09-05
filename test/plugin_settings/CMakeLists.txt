add_executable(PluginSettingsTest
  PluginSettingsTest.cpp
  ${REPO_ROOT}/src/plugins/PluginSettings.cpp
)
target_include_directories(PluginSettingsTest PRIVATE stubs)
target_compile_definitions(PluginSettingsTest PRIVATE ENABLE_PLUGINS=1)
target_link_libraries(PluginSettingsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PluginSettingsTest)
