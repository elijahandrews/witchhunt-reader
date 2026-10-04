add_executable(FontManifestParserTest
  FontManifestParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/FontManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(FontManifestParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
)

target_compile_definitions(FontManifestParserTest PRIVATE
  FONT_MANIFEST_PATH="${REPO_ROOT}/assets/sd-fonts/fonts.json"
)

target_link_libraries(FontManifestParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontManifestParserTest)
