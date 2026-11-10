# Production converters and pinned raster libraries; no simulated decoder output.
set(PNGDEC_SOURCE_DIR "" CACHE PATH "Existing PNGdec 1.1.6 source for image backend tests")
if(NOT PNGDEC_SOURCE_DIR)
  foreach(profile picco x4-pro default)
    if(EXISTS "${REPO_ROOT}/.pio/libdeps/${profile}/PNGdec/src/PNGdec.cpp")
      set(PNGDEC_SOURCE_DIR "${REPO_ROOT}/.pio/libdeps/${profile}/PNGdec")
      break()
    endif()
  endforeach()
endif()
if(NOT PNGDEC_SOURCE_DIR)
  FetchContent_Declare(pngdec
    GIT_REPOSITORY https://github.com/bitbank2/PNGdec.git
    GIT_TAG 1.1.6
    SOURCE_SUBDIR crossink-host-tests)
  FetchContent_MakeAvailable(pngdec)
  set(PNGDEC_SOURCE_DIR "${pngdec_SOURCE_DIR}")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/patch_pngdec.py ${PNGDEC_SOURCE_DIR}
  RESULT_VARIABLE png_patch_result)
if(NOT png_patch_result EQUAL 0)
  message(FATAL_ERROR "PNGdec host dependency patch failed")
endif()
add_library(pngdec_host STATIC
  ${PNGDEC_SOURCE_DIR}/src/PNGdec.cpp
  ${PNGDEC_SOURCE_DIR}/src/adler32.c ${PNGDEC_SOURCE_DIR}/src/crc32.c
  ${PNGDEC_SOURCE_DIR}/src/infback.c ${PNGDEC_SOURCE_DIR}/src/inffast.c
  ${PNGDEC_SOURCE_DIR}/src/inflate.c ${PNGDEC_SOURCE_DIR}/src/inftrees.c
  ${PNGDEC_SOURCE_DIR}/src/zutil.c)
target_include_directories(pngdec_host PUBLIC ${PNGDEC_SOURCE_DIR}/src)
target_compile_definitions(pngdec_host PUBLIC __LINUX__ PNG_MAX_BUFFERED_PIXELS=16416)

add_executable(ImageBackendTest SvgDecoderTest.cpp ImageBackendTest.cpp
  ${REPO_ROOT}/lib/ImageFormatDetector/ImageFormatDetector.cpp
  ${REPO_ROOT}/lib/SvgRasterizer/SvgRasterizer.cpp
  ${REPO_ROOT}/lib/ProgressiveJpeg/ProgressiveJpeg.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/SvgToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDecoderFactory.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/PngToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/GifToFramebufferConverter.cpp)
target_include_directories(ImageBackendTest PRIVATE stubs
  ${REPO_ROOT}/lib/ImageFormatDetector ${REPO_ROOT}/lib/SvgRasterizer ${REPO_ROOT}/lib/Yxml
  ${REPO_ROOT}/lib/ProgressiveJpeg
  ${REPO_ROOT}/lib/Epub/Epub/converters ${REPO_ROOT}/lib/GifSupport
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget)
target_link_libraries(ImageBackendTest PRIVATE crosspoint_test_common GTest::gtest_main
  jpegdec_host animatedgif_host pngdec_host SaxParser)
gtest_discover_tests(ImageBackendTest)
add_test(NAME PatchPngdec COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/image_backends/test_patch_pngdec.py)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_patch_pngdec.py --extract
  ${PNGDEC_SOURCE_DIR}/src/inflate.c ${CMAKE_CURRENT_BINARY_DIR}/PngInflateCopy.inc
  RESULT_VARIABLE png_extract_result)
if(NOT png_extract_result EQUAL 0)
  message(FATAL_ERROR "Could not extract production PNG LZ copy")
endif()
add_executable(PngCopyBoundsTest PngCopyBoundsTest.cpp)
target_include_directories(PngCopyBoundsTest PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(PngCopyBoundsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PngCopyBoundsTest)
