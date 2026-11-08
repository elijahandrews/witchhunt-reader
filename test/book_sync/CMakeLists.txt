add_executable(BookSyncProtocolTest BookSyncProtocolTest.cpp)
target_link_libraries(BookSyncProtocolTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookSyncProtocolTest)

# Production activity and filesystem orchestration with a FAT-like card and controlled transport.
# OpenSSL supplies only host SHA-256; the X4 Pro uses its hardware-backed SDK API.
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_executable(BookSyncActivityTest
  BookSyncActivityTest.cpp
  ${REPO_ROOT}/src/activities/network/BookSyncActivity.cpp
)
target_include_directories(BookSyncActivityTest PRIVATE
  stubs
  ${REPO_ROOT}/src
)
target_compile_definitions(BookSyncActivityTest PRIVATE CONFIG_IDF_TARGET_ESP32S3=1)
target_compile_options(BookSyncActivityTest PRIVATE -Wno-deprecated-declarations)
target_link_libraries(BookSyncActivityTest PRIVATE crosspoint_test_common ArduinoJson OpenSSL::Crypto GTest::gtest_main)
gtest_discover_tests(BookSyncActivityTest)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  foreach(target BookSyncProtocolTest BookSyncActivityTest)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()
