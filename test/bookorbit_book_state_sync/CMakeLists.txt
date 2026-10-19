add_executable(BookOrbitBookStateSyncDateTest
  BookOrbitBookStateSyncDateTest.cpp
  BookOrbitBookStateStoreTest.cpp
  ${REPO_ROOT}/src/BookOrbitBookStateSync.cpp
  ${REPO_ROOT}/src/BookOrbitBookStateStore.cpp
  ${REPO_ROOT}/src/ReadingStatsSlotFile.cpp
  ${REPO_ROOT}/lib/hal/StagedFileReplace.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(BookOrbitBookStateSyncDateTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/BookOrbitCatalog
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/test/bookorbit_bookmark_sync
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib
)
target_compile_options(BookOrbitBookStateSyncDateTest PRIVATE -ffunction-sections -fdata-sections)
target_compile_definitions(BookOrbitBookStateSyncDateTest PRIVATE ENABLE_SERIAL_LOG=1 LOG_LEVEL=0)
if(MINGW)
  target_compile_definitions(BookOrbitBookStateSyncDateTest PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS)
endif()
target_link_libraries(BookOrbitBookStateSyncDateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
if(APPLE)
  target_link_options(BookOrbitBookStateSyncDateTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(BookOrbitBookStateSyncDateTest PRIVATE -Wl,--gc-sections)
endif()

gtest_discover_tests(BookOrbitBookStateSyncDateTest)
