add_executable(BookOrbitReadingStatsOutboxTest
  BookOrbitReadingStatsOutboxTest.cpp
  ${REPO_ROOT}/src/BookOrbitReadingStatsOutbox.cpp
  ${REPO_ROOT}/src/ReadingStatsSlotFile.cpp
  ${REPO_ROOT}/lib/hal/StagedFileReplace.cpp
)
target_include_directories(BookOrbitReadingStatsOutboxTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/hal
)
target_link_libraries(BookOrbitReadingStatsOutboxTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BookOrbitReadingStatsOutboxTest)
add_executable(ReadingSessionTrackerTest
  ReadingSessionTrackerTest.cpp
  ${REPO_ROOT}/src/ReadingSessionTracker.cpp
  ${REPO_ROOT}/src/BookOrbitReadingStatsOutbox.cpp
  ${REPO_ROOT}/src/ReadingStatsSlotFile.cpp
  ${REPO_ROOT}/lib/hal/StagedFileReplace.cpp
)
target_include_directories(ReadingSessionTrackerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/BookOrbitCatalog
  ${REPO_ROOT}/lib/hal
)
target_link_libraries(ReadingSessionTrackerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(ReadingSessionTrackerTest)
