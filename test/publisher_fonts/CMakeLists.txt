# Use the original generated font outlines already embedded in the checked-in
# test EPUB. Extraction needs only Python's standard library.
set(PUBLISHER_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
add_custom_command(OUTPUT ${PUBLISHER_FIXTURES}/stamp
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/extract_fixtures.py
          --epub ${REPO_ROOT}/test/epubs/test_css_typography.epub --out ${PUBLISHER_FIXTURES}
  DEPENDS extract_fixtures.py ${REPO_ROOT}/test/epubs/test_css_typography.epub
  VERBATIM)
add_custom_target(publisher_fixture_fonts DEPENDS ${PUBLISHER_FIXTURES}/stamp)
file(GLOB PUBLISHER_FT_SOURCES CONFIGURE_DEPENDS ${REPO_ROOT}/lib/FreeType/src/*.c)
add_library(publisher_freetype_host STATIC ${PUBLISHER_FT_SOURCES})
target_include_directories(publisher_freetype_host PUBLIC
  ${REPO_ROOT}/lib/FreeType/config ${REPO_ROOT}/lib/FreeType/third_party/freetype/include)
target_compile_definitions(publisher_freetype_host PUBLIC WITCH_FONT_AUTOHINT=1
  "FT_CONFIG_OPTIONS_H=<witch_ftoption.h>" "FT_CONFIG_MODULES_H=<witch_ftmodule.h>")
add_executable(PublisherFontsTest PublisherFontsTest.cpp
  ${REPO_ROOT}/lib/EpubFonts/EpubFontManager.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssFontCatalog.cpp
  ${REPO_ROOT}/lib/OutlineFont/OutlineFontFace.cpp
  ${REPO_ROOT}/lib/OutlineFont/Caps.cpp
  ${REPO_ROOT}/lib/OutlineFont/SdkGpos.cpp
  ${REPO_ROOT}/lib/OutlineFont/SdkGsub.cpp
  ${REPO_ROOT}/lib/ReaderMono/ReaderMono.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp)
add_dependencies(PublisherFontsTest publisher_fixture_fonts)
target_include_directories(PublisherFontsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${REPO_ROOT}/test/zip_entry_reader ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/EpubFonts ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/OutlineFont
  ${REPO_ROOT}/lib/ReaderMono ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/Logging)
target_compile_definitions(PublisherFontsTest PRIVATE PUBLISHER_FIXTURE_DIR="${PUBLISHER_FIXTURES}")
target_link_libraries(PublisherFontsTest PRIVATE publisher_freetype_host crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PublisherFontsTest)
