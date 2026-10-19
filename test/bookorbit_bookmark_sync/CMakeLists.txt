add_executable(KOReaderWorkerOwnershipTest KOReaderWorkerOwnershipTest.cpp)
target_include_directories(KOReaderWorkerOwnershipTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/epub_pipeline
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/BookOrbitCatalog
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/hal
)
target_compile_definitions(KOReaderWorkerOwnershipTest PRIVATE
  FREEINK_MCU_S3=1
  BOARD_HAS_PSRAM=1
  ARDUINOJSON_ENABLE_ARDUINO_STRING=1
)
target_link_libraries(KOReaderWorkerOwnershipTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(KOReaderWorkerOwnershipTest)

file(GLOB ARDUINOJSON_INCLUDE_DIRS "${REPO_ROOT}/.pio/libdeps/*/ArduinoJson/src")
if(NOT ARDUINOJSON_INCLUDE_DIRS)
  message(FATAL_ERROR "Run PlatformIO once to download ArduinoJson before building the bookmark client smoke")
endif()
list(GET ARDUINOJSON_INCLUDE_DIRS 0 ARDUINOJSON_INCLUDE_DIR)
target_include_directories(KOReaderWorkerOwnershipTest PRIVATE ${ARDUINOJSON_INCLUDE_DIR})
target_compile_options(KOReaderWorkerOwnershipTest PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(KOReaderWorkerOwnershipTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(KOReaderWorkerOwnershipTest PRIVATE -Wl,--gc-sections)
endif()

add_library(BookOrbitBookmarkClientObjects OBJECT
  ${REPO_ROOT}/lib/BookOrbitCatalog/BookOrbitCatalogClient.cpp
  ${REPO_ROOT}/lib/SecureNet/src/SecureHttpClient.cpp
  ${REPO_ROOT}/lib/SecureNet/src/HttpRange.cpp
  ${REPO_ROOT}/lib/SecureNet/src/SecureClient.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)
target_include_directories(BookOrbitBookmarkClientObjects PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/BookOrbitCatalog
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/SecureNet/include
  ${ARDUINOJSON_INCLUDE_DIR}
)
target_compile_definitions(BookOrbitBookmarkClientObjects PRIVATE
  FREEINK_DEVICE_X4=1
  CROSSPOINT_VERSION="native-test"
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=0
)
target_compile_options(BookOrbitBookmarkClientObjects PRIVATE -ffunction-sections -fdata-sections)
target_sources(EpubPipelineTest PRIVATE $<TARGET_OBJECTS:BookOrbitBookmarkClientObjects>)
target_include_directories(EpubPipelineTest PRIVATE ${REPO_ROOT}/lib/JsonParser)
if(APPLE)
  target_link_options(EpubPipelineTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(EpubPipelineTest PRIVATE -Wl,--gc-sections)
endif()
