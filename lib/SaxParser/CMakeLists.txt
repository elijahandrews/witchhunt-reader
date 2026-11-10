add_library(SaxParser STATIC SaxParserYxml.cpp ../Yxml/yxml.c)
target_include_directories(SaxParser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../Yxml)
