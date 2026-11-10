find_package(EXPAT REQUIRED)
# Compile the production translation unit unchanged apart from its Epub class
# boundary. The harness replaces unrelated metadata/UI and the SDK decryptor;
# ZIP validation, manifest parsing and Adobe prefix transformation remain real.
set(EPUB_FONT_SOURCE ${REPO_ROOT}/lib/Epub/EpubFontContents.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EPUB_FONT_SOURCE})
file(READ ${EPUB_FONT_SOURCE} EPUB_FONT_CONTENTS)
string(REPLACE "#include \"Epub.h\"" "#include \"EpubFontHarness.h\"" EPUB_FONT_CONTENTS "${EPUB_FONT_CONTENTS}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/EpubFontContents.cpp "${EPUB_FONT_CONTENTS}")
add_executable(EpubFontContentsTest EpubFontContentsTest.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/EpubFontContents.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/AdobeFontObfuscation.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp)
target_include_directories(EpubFontContentsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${REPO_ROOT}/lib/ZipFile ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/test/book_text_search/stubs ${REPO_ROOT}/test/book_text_search
  ${REPO_ROOT}/test/pxc_v2/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/XmlParserUtils ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/ZipFile ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget)
target_link_libraries(EpubFontContentsTest PRIVATE crosspoint_test_common GTest::gtest_main EXPAT::EXPAT)
gtest_discover_tests(EpubFontContentsTest)
