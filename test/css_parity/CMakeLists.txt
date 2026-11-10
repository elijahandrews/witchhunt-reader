add_executable(CssParityTest CssParityTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssFontCatalog.cpp)
target_include_directories(CssParityTest PRIVATE ${REPO_ROOT}/test/memory_policy/stubs
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget ${REPO_ROOT}/lib/Epub/Epub/css)
target_compile_definitions(CssParityTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(CssParityTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CssParityTest)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(CssParityTest PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(CssParityTest PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME DocumentStylesIntegration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_document_styles.py)
