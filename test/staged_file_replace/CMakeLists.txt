add_executable(StagedFileReplaceTest
  StagedFileReplaceTest.cpp
  ${REPO_ROOT}/lib/hal/StagedFileReplace.cpp
)
target_include_directories(StagedFileReplaceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/hal
)
target_link_libraries(StagedFileReplaceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(StagedFileReplaceTest)
