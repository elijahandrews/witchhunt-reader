add_executable(BookTextSearchTest
  BookTextSearchTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookTextSearch.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(BookTextSearchTest PRIVATE
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Utf8)
target_link_libraries(BookTextSearchTest PRIVATE crosspoint_test_common GTest::gtest_main expat)
gtest_discover_tests(BookTextSearchTest)

add_executable(BookSearchSessionTest
  BookSearchSessionTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookSearchSession.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookTextSearch.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp)
target_include_directories(BookSearchSessionTest PRIVATE
  stubs ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/test/pxc_v2/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget)
target_link_libraries(BookSearchSessionTest PRIVATE crosspoint_test_common GTest::gtest_main expat)
gtest_discover_tests(BookSearchSessionTest)

# HTML matching uses the same bounded SaxParser backend on host and firmware.
