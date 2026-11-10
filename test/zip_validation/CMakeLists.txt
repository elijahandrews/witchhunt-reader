add_executable(ZipValidationTest ZipValidationTest.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp)
target_include_directories(ZipValidationTest PRIVATE
  ${REPO_ROOT}/test/book_text_search/stubs ${REPO_ROOT}/test/book_text_search
  ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/test/pxc_v2/stubs
  ${REPO_ROOT}/lib/ZipFile ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget)
target_link_libraries(ZipValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ZipValidationTest)
