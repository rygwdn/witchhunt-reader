# The firmware's ArduinoJson (platformio.ini pins bblanchon/ArduinoJson @ 7.4.2), header-only.
include(FetchContent)
FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(arduinojson)

# PlatformIO's offline package contains the headers but omits the upstream CMake project.
if(NOT TARGET ArduinoJson AND EXISTS "${arduinojson_SOURCE_DIR}/src/ArduinoJson.h")
  add_library(ArduinoJson INTERFACE)
  target_include_directories(ArduinoJson INTERFACE "${arduinojson_SOURCE_DIR}/src")
endif()

add_executable(KOReaderProgressReplyTest
  KOReaderProgressReplyTest.cpp
)

target_include_directories(KOReaderProgressReplyTest PRIVATE
  # HardwareSerial.h for Logging.h; the LOG_* macros compile to nothing here.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderProgressReplyTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(KOReaderProgressReplyTest)
