cmake_minimum_required(VERSION 3.16)
project(ScalableFontTests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../freeink-sdk/libs/font/FreeInkFont)
file(GLOB SOURCES ${SDK}/src/*.c ${SDK}/src/*.cpp ${SDK}/src/freetype/*.c)
add_executable(scalable_font_test ScalableFontTest.cpp ${SOURCES})
target_compile_options(scalable_font_test PRIVATE -UNDEBUG)
target_include_directories(scalable_font_test PRIVATE
  ${SDK}/include
  ${SDK}/third_party/freetype/include
  ${SDK}/third_party/stb)
target_compile_definitions(scalable_font_test PRIVATE
  FREEINK_FONT_ENABLE_AUTOHINT=1
  FREEINK_FONT_ENABLE_NATIVE_HINTING=1
  FREEINK_FONT_ENABLE_MONOCHROME=1)

enable_testing()

function(add_scalable_font_sizing_test target expected_ppi)
  add_executable(${target} ScalableFontSizingTest.cpp)
  target_compile_definitions(${target} PRIVATE EXPECTED_PANEL_PPI=${expected_ppi} ${ARGN})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

add_scalable_font_sizing_test(scalable_font_sizing_default 150)
add_scalable_font_sizing_test(scalable_font_sizing_sticky 150 FREEINK_DEVICE_STICKY=1)
add_scalable_font_sizing_test(scalable_font_sizing_x4_pro 150 FREEINK_DEVICE_X4PRO=1)
add_scalable_font_sizing_test(scalable_font_sizing_x4_classic 150 FREEINK_DEVICE_X4CLASSIC=1)
add_scalable_font_sizing_test(scalable_font_sizing_sticky_simulator 150 SIMULATOR_DEVICE_STICKY=1)
add_scalable_font_sizing_test(scalable_font_sizing_x4_pro_simulator 150 SIMULATOR_DEVICE_X4_PRO=1)
add_scalable_font_sizing_test(scalable_font_sizing_x4_classic_simulator 150 SIMULATOR_DEVICE_X4_CLASSIC=1)

# Compile the rasterizer separately without sanitizer instrumentation so its
# production-sized stack frames can be checked accurately.
add_library(scalable_font_stack_check OBJECT
  ${SDK}/src/freetype/ft_autofit.c
  ${SDK}/src/freetype/ft_raster.c)
target_compile_options(scalable_font_stack_check PRIVATE -O2)
target_include_directories(scalable_font_stack_check PRIVATE
  ${SDK}/include
  ${SDK}/third_party/freetype/include)
target_compile_definitions(scalable_font_stack_check PRIVATE
  FREEINK_FONT_ENABLE_AUTOHINT=1
  FREEINK_FONT_ENABLE_NATIVE_HINTING=1
  FREEINK_FONT_ENABLE_MONOCHROME=1)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
 target_compile_options(scalable_font_stack_check PRIVATE -Wframe-larger-than=2048 -Werror=frame-larger-than)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
 target_compile_options(scalable_font_stack_check PRIVATE -Werror=frame-larger-than=2048)
endif()
option(SANITIZE "Address/undefined behavior checks" ON)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
 target_compile_options(scalable_font_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
 target_link_options(scalable_font_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME scalable_fonts COMMAND scalable_font_test ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/EpdFont/scalableFonts)

add_executable(scalable_font_adapter_test ScalableFontAdapterTest.cpp
  ../../lib/ScalableFont/HalScalableFont.cpp ${SOURCES})
set_property(TARGET scalable_font_adapter_test PROPERTY CXX_STANDARD 20)
target_compile_options(scalable_font_adapter_test PRIVATE -UNDEBUG)
target_include_directories(scalable_font_adapter_test PRIVATE
  stubs ../../lib/ScalableFont ../../lib/EpdFont ../../lib/Memory
  ${SDK}/include ${SDK}/third_party/freetype/include ${SDK}/third_party/stb)
target_compile_definitions(scalable_font_adapter_test PRIVATE
  CROSSINK_SCALABLE_FONTS=1 SIMULATOR=1
  FREEINK_FONT_ENABLE_AUTOHINT=1 FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(scalable_font_adapter_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(scalable_font_adapter_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME scalable_font_adapter COMMAND scalable_font_adapter_test
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/EpdFont/scalableFonts
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/EpdFont/scalableFonts/bitter_regular.ttf
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/EpdFont/scalableFonts/bitter_bold.ttf
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/EpdFont/scalableFonts/bitter_italic.ttf
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/EpdFont/scalableFonts/bitter_bolditalic.ttf)

# Takes original and subset font directories; kept out of CTest because the
# original binary assets are extracted from the requested baseline revision.
add_executable(scalable_font_subset_compare ScalableFontSubsetCompare.cpp ${SOURCES})
target_compile_options(scalable_font_subset_compare PRIVATE -UNDEBUG)
target_include_directories(scalable_font_subset_compare PRIVATE
  ${SDK}/include ${SDK}/third_party/freetype/include ${SDK}/third_party/stb)
target_compile_definitions(scalable_font_subset_compare PRIVATE
  FREEINK_FONT_ENABLE_AUTOHINT=1 FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(scalable_font_subset_compare PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(scalable_font_subset_compare PRIVATE -fsanitize=address,undefined)
endif()

add_executable(font_byte_digest_test FontByteDigestTest.cpp)
target_compile_options(font_byte_digest_test PRIVATE -UNDEBUG)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(font_byte_digest_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(font_byte_digest_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME font_byte_digest COMMAND font_byte_digest_test)

# Original runtime-generated geometric fonts verify authored glyph IDs survive
# the native adapter and final-grid raster without synthetic small-cap scaling.
add_executable(scalable_font_caps_test ScalableFontCapsTest.cpp ../../lib/ReaderMono/ReaderMono.cpp
  ../../lib/ScalableFont/HalScalableFont.cpp ../../lib/EpdFont/EpdFont.cpp
  ../../lib/EpdFont/EpdFontFamily.cpp ../../lib/Utf8/Utf8.cpp ${SOURCES})
set_property(TARGET scalable_font_caps_test PROPERTY CXX_STANDARD 20)
target_compile_options(scalable_font_caps_test PRIVATE -UNDEBUG)
target_include_directories(scalable_font_caps_test PRIVATE ../../lib/ReaderMono stubs ../../lib/ScalableFont ../../lib/EpdFont
  ../../lib/Memory ../../lib/Utf8 ${SDK}/include ${SDK}/third_party/freetype/include ${SDK}/third_party/stb)
target_compile_definitions(scalable_font_caps_test PRIVATE CROSSINK_SCALABLE_FONTS=1 SIMULATOR=1 FREEINK_FONT_ENABLE_CFF=1
  FREEINK_FONT_ENABLE_AUTOHINT=1 FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(scalable_font_caps_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(scalable_font_caps_test PRIVATE -fsanitize=address,undefined)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME scalable_font_caps COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/run_caps_fixture.py $<TARGET_FILE:scalable_font_caps_test>)

add_executable(scalable_font_variable_test ScalableFontVariableTest.cpp
  ../../lib/ScalableFont/HalScalableFont.cpp ../../lib/EpdFont/EpdFont.cpp
  ../../lib/EpdFont/EpdFontFamily.cpp ../../lib/Utf8/Utf8.cpp ${SOURCES})
set_property(TARGET scalable_font_variable_test PROPERTY CXX_STANDARD 20)
target_compile_options(scalable_font_variable_test PRIVATE -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(scalable_font_variable_test PRIVATE -fsanitize=address,undefined)
target_include_directories(scalable_font_variable_test PRIVATE stubs ../../lib/ScalableFont ../../lib/EpdFont
  ../../lib/Memory ../../lib/Utf8 ${SDK}/include ${SDK}/third_party/freetype/include ${SDK}/third_party/stb)
target_compile_definitions(scalable_font_variable_test PRIVATE CROSSINK_SCALABLE_FONTS=1 SIMULATOR=1
  FREEINK_FONT_ENABLE_CFF=1 FREEINK_FONT_ENABLE_AUTOHINT=1 FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1)
add_test(NAME scalable_font_variable COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/run_variable_fixture.py $<TARGET_FILE:scalable_font_variable_test>)
add_executable(scalable_gpos_variation_test GposVariationTest.cpp ${SDK}/src/Gpos.cpp)
target_compile_features(scalable_gpos_variation_test PRIVATE cxx_std_17)
target_include_directories(scalable_gpos_variation_test PRIVATE ${SDK}/include)
target_compile_options(scalable_gpos_variation_test PRIVATE -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(scalable_gpos_variation_test PRIVATE -fsanitize=address,undefined)
add_test(NAME scalable_gpos_variation COMMAND scalable_gpos_variation_test)

get_target_property(GENERIC_SOURCES scalable_font_caps_test SOURCES)
list(REMOVE_ITEM GENERIC_SOURCES ScalableFontCapsTest.cpp)
get_target_property(GENERIC_INCLUDES scalable_font_caps_test INCLUDE_DIRECTORIES)
get_target_property(GENERIC_DEFINITIONS scalable_font_caps_test COMPILE_DEFINITIONS)
add_executable(scalable_generic_font_test ScalableGenericFontTest.cpp ../../lib/ScalableFont/ScalableBuiltins.cpp ${GENERIC_SOURCES})
set_property(TARGET scalable_generic_font_test PROPERTY CXX_STANDARD 20)
target_include_directories(scalable_generic_font_test PRIVATE generic_stubs ${GENERIC_INCLUDES})
target_compile_definitions(scalable_generic_font_test PRIVATE ${GENERIC_DEFINITIONS})
target_compile_options(scalable_generic_font_test PRIVATE -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(scalable_generic_font_test PRIVATE -fsanitize=address,undefined)
add_test(NAME scalable_generic_font COMMAND scalable_generic_font_test)

add_executable(scalable_size_cache_test ScalableSizeCacheTest.cpp)
target_include_directories(scalable_size_cache_test PRIVATE cache_stubs ../../lib/ScalableFont)
target_compile_definitions(scalable_size_cache_test PRIVATE CROSSINK_SCALABLE_FONTS=1)
target_compile_options(scalable_size_cache_test PRIVATE -UNDEBUG)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(scalable_size_cache_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(scalable_size_cache_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME scalable_size_cache COMMAND scalable_size_cache_test)
