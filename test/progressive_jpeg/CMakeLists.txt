find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PROGRESSIVE_FIXTURE_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH "Python with Pillow for synthetic progressive JPEG fixtures")
execute_process(COMMAND ${PROGRESSIVE_FIXTURE_PYTHON} -c "from PIL import Image"
  RESULT_VARIABLE pillow_result ERROR_QUIET)
if(NOT pillow_result EQUAL 0)
  message(FATAL_ERROR "Progressive JPEG tests need Pillow. Set PROGRESSIVE_FIXTURE_PYTHON to a Python interpreter with Pillow.")
endif()
set(PROGRESSIVE_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
execute_process(COMMAND ${PROGRESSIVE_FIXTURE_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/generate_fixtures.py
  ${PROGRESSIVE_FIXTURE_DIR} COMMAND_ERROR_IS_FATAL ANY)
if(TARGET ImageBackendTest)
  target_compile_definitions(ImageBackendTest PRIVATE PROGRESSIVE_FIXTURE_DIR="${PROGRESSIVE_FIXTURE_DIR}")
endif()
add_executable(ProgressiveJpegTest ProgressiveJpegTest.cpp
  ${REPO_ROOT}/lib/ProgressiveJpeg/ProgressiveJpeg.cpp)
target_include_directories(ProgressiveJpegTest PRIVATE ../image_backends/stubs
  ${REPO_ROOT}/lib/ProgressiveJpeg ${REPO_ROOT}/lib/Memory)
target_compile_definitions(ProgressiveJpegTest PRIVATE FIXTURE_DIR="${PROGRESSIVE_FIXTURE_DIR}")
target_link_libraries(ProgressiveJpegTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ProgressiveJpegTest)
