add_executable(PersistableStoreAtomicTest
  PersistableStoreAtomicTest.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
)

target_include_directories(PersistableStoreAtomicTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(PersistableStoreAtomicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(PersistableStoreAtomicTest)

add_executable(CrossPointStateTest
  CrossPointStateTest.cpp
  ${REPO_ROOT}/src/CrossPointState.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(CrossPointStateTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/uzlib/src
)
target_link_libraries(CrossPointStateTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(CrossPointStateTest)

add_executable(wifi_migration WifiMigrationTest.cpp
  ${REPO_ROOT}/src/WifiCredentialStore.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp)
target_include_directories(wifi_migration PRIVATE stubs ${REPO_ROOT}/src ${REPO_ROOT}/lib/Serialization)
target_link_libraries(wifi_migration PRIVATE crosspoint_test_common ArduinoJson)
target_compile_options(wifi_migration PRIVATE -UNDEBUG)
foreach(scenario legacy corrupt current backup)
  add_test(NAME wifi_migration_${scenario} COMMAND wifi_migration ${scenario})
endforeach()

add_executable(recent_books_migration RecentBooksMigrationTest.cpp
  ${REPO_ROOT}/src/RecentBooksStore.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp)
target_include_directories(recent_books_migration PRIVATE recent_stubs stubs
  ${REPO_ROOT}/src ${REPO_ROOT}/lib/Serialization)
target_link_libraries(recent_books_migration PRIVATE crosspoint_test_common ArduinoJson)
target_compile_options(recent_books_migration PRIVATE -UNDEBUG)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(recent_books_migration PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(recent_books_migration PRIVATE -fsanitize=address,undefined)
endif()
foreach(scenario legacy backup both current own_backup corrupt copy_failure)
  add_test(NAME recent_books_migration_${scenario} COMMAND recent_books_migration ${scenario})
endforeach()
