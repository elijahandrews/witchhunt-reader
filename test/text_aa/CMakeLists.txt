add_executable(text_aa TextAaTest.cpp)
target_include_directories(text_aa PRIVATE stubs ${REPO_ROOT}/src)
target_compile_options(text_aa PRIVATE -UNDEBUG)
add_test(NAME text_aa COMMAND text_aa)
