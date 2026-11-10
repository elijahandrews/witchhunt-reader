find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/extract_cover.py
  ${REPO_ROOT}/lib/Epub/Epub.cpp ${CMAKE_CURRENT_BINARY_DIR} COMMAND_ERROR_IS_FATAL ANY)
add_executable(EpubMetadataTest EpubMetadataTest.cpp CoverSelectionTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(EpubMetadataTest PRIVATE stubs ../section_persistence/stubs
  ${CMAKE_CURRENT_BINARY_DIR}
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Utf8)
target_link_libraries(EpubMetadataTest PRIVATE crosspoint_test_common GTest::gtest_main SaxParser)
gtest_discover_tests(EpubMetadataTest)
