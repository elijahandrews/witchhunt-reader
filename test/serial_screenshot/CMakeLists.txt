add_test(NAME SerialScreenshot
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py
          --out ${CMAKE_CURRENT_BINARY_DIR}/probe)
set_tests_properties(SerialScreenshot PROPERTIES TIMEOUT 60)
