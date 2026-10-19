add_executable(SvgDecoderTest
  SvgDecoderTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/CooperativeAbort/CooperativeAbort.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/SvgRasterizer.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/SvgToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)
target_include_directories(SvgDecoderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Epub/Epub/converters
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib
)
target_link_libraries(SvgDecoderTest PRIVATE crosspoint_test_common SaxParser GTest::gtest_main)
gtest_discover_tests(SvgDecoderTest)
