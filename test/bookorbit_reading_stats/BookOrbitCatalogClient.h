#pragma once

#include <cstdint>
#include <string>
#include <vector>

class BookOrbitCatalogClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    NOT_SUPPORTED,
  };
  enum class ReadStatus : uint8_t { Unread = 0, WantToRead, Reading, OnHold, Read, Abandoned };
};

struct BookOrbitPageStatEvent {
  uint32_t page = 0;
  uint32_t startTime = 0;
  uint32_t durationSeconds = 0;
  uint32_t totalPages = 0;
};

struct BookOrbitPageStatsBook {
  std::string hash;
  int32_t bookFileId = -1;
  std::vector<BookOrbitPageStatEvent> events;
};
