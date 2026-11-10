add_executable(HeadMediaInventoryTest HeadMediaInventoryTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/HeadMediaInventory.cpp)
target_include_directories(HeadMediaInventoryTest PRIVATE ${REPO_ROOT}/lib/Epub)
target_link_libraries(HeadMediaInventoryTest PRIVATE crosspoint_test_common GTest::gtest_main SaxParser)
gtest_discover_tests(HeadMediaInventoryTest)
add_test(NAME HeadMediaInventoryIntegration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_inventory_integration.py)
