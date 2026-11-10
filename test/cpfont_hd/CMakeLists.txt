add_executable(cpfont_hd CpFontHdTest.cpp
  ../../lib/EpdFont/SdCardFont.cpp ../../lib/EpdFont/EpdFont.cpp
  ../../lib/EpdFont/EpdFontFamily.cpp ../../lib/Utf8/Utf8.cpp)
target_compile_features(cpfont_hd PRIVATE cxx_std_20)
target_include_directories(cpfont_hd PRIVATE ../sd_card_font_intervals/stubs
  ../../lib/EpdFont ../../lib/Memory ../../lib/MemoryBudget ../../lib/Utf8)
target_compile_options(cpfont_hd PRIVATE -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(cpfont_hd PRIVATE -fsanitize=address,undefined)
add_test(NAME cpfont_hd COMMAND cpfont_hd)
