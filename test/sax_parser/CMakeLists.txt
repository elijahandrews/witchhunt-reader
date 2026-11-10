add_executable(SaxParserTest
  SaxParserTest.cpp
)


target_link_libraries(SaxParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  SaxParser
)

gtest_discover_tests(SaxParserTest)
