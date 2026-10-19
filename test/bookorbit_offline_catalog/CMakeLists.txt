add_executable(BookOrbitCatalogStoreTest
  BookOrbitCatalogStoreTest.cpp
  ${REPO_ROOT}/lib/BookOrbitCatalog/BookOrbitCatalogStore.cpp
)
target_include_directories(BookOrbitCatalogStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/BookOrbitCatalog
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(BookOrbitCatalogStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookOrbitCatalogStoreTest)
