add_executable(SdFontManagerTest
  SdFontManagerTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontManager.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)
target_include_directories(SdFontManagerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)
target_link_libraries(SdFontManagerTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SdFontManagerTest)
