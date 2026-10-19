# The download loop, and where SecureHttpClient sends credentials, over a scripted network. The
# stubs (Arduino, Client, WiFiClient, WiFi, base64, Logging) come first on the include path; the stub
# SecureClient is in RangeDownloadTest.cpp.
add_executable(RangeDownloadTest
  RangeDownloadTest.cpp
  CredentialRedirectTest.cpp
  HttpDownloaderTest.cpp
  ${REPO_ROOT}/src/network/HttpDownloader.cpp
  ${REPO_ROOT}/lib/hal/StagedFileReplace.cpp
  ${REPO_ROOT}/lib/SecureNet/src/RangeDownload.cpp
  ${REPO_ROOT}/lib/SecureNet/src/SecureHttpClient.cpp
  ${REPO_ROOT}/lib/SecureNet/src/HttpRange.cpp
)

target_include_directories(RangeDownloadTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/SecureNet/include
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/bookorbit_reading_stats
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/src/network
)

target_link_libraries(RangeDownloadTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_compile_definitions(RangeDownloadTest PRIVATE CROSSPOINT_VERSION="host-test")

gtest_discover_tests(RangeDownloadTest)
