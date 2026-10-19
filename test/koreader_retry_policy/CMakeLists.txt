add_executable(KOReaderRetryPolicyTest KOReaderRetryPolicyTest.cpp)

target_include_directories(KOReaderRetryPolicyTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderRetryPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderRetryPolicyTest)
