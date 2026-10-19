add_executable(BookOrbitCatalogTest
  BookOrbitCatalogListParserTest.cpp
  BookOrbitCatalogSnapshotParserTest.cpp
  ${REPO_ROOT}/lib/BookOrbitCatalog/BookOrbitCatalogListParser.cpp
  ${REPO_ROOT}/lib/BookOrbitCatalog/BookOrbitCatalogSnapshotParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitCatalogTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbitCatalog
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitCatalogTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitCatalogTest)
