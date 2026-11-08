FetchContent_GetProperties(arduinojson)

add_test(NAME StatusBarShortcut
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py
          --out ${CMAKE_CURRENT_BINARY_DIR}/routing)
add_test(NAME StatusBarSettings
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/settings.py
          --out ${CMAKE_CURRENT_BINARY_DIR}/settings
          --arduino-json ${arduinojson_SOURCE_DIR}/src)
set_tests_properties(StatusBarShortcut StatusBarSettings PROPERTIES TIMEOUT 60)
