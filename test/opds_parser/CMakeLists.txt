add_executable(OpdsParserTest
  OpdsParserTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(OpdsParserTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(OpdsParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  SaxParser
)

gtest_discover_tests(OpdsParserTest)

add_executable(OpdsJsonParserTest
  OpdsJsonParserTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsJsonParser.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsJsonTokenizer.cpp
)
target_include_directories(OpdsJsonParserTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/JsonParser
)
target_link_libraries(OpdsJsonParserTest PRIVATE crosspoint_test_common GTest::gtest_main SaxParser)
gtest_discover_tests(OpdsJsonParserTest)
