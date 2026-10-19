#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct BookOrbitBookmarkKey {
  std::array<char, 33> key{};
  std::array<char, 20> datetime{};
};

struct BookOrbitBookmarkChange {
  std::string datetime;
  std::string datetimeUpdated;
  std::string position;
  uint32_t page = 0;
  std::string chapter;
  std::string note;
};

struct BookOrbitBookmarkExchangeBook {
  std::string hash;
  std::vector<BookOrbitBookmarkKey> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmarkChange> changes;
};

struct BookOrbitBookmarkExchangeRequest {
  std::vector<BookOrbitBookmarkExchangeBook> books;
};

struct BookOrbitBookmarkAdd {
  int32_t serverId = -1;
  std::string position;
  int32_t page = -1;
  std::string title;
};

struct BookOrbitBookmarkDelete {
  int32_t serverId = -1;
  std::string key;
  std::string datetime;
};

struct BookOrbitBookmarkExchangeResult {
  std::string hash;
  std::vector<BookOrbitBookmarkAdd> add;
  std::vector<BookOrbitBookmarkDelete> remove;
  bool more = false;
};

struct BookOrbitBookmarkExchangeResponse {
  std::vector<BookOrbitBookmarkExchangeResult> results;
  std::vector<std::string> unmatched;
};

struct BookOrbitBookmarkAckApplied {
  int32_t serverId = -1;
  bool applied = false;
  std::string key;
  std::string datetime;
  std::string position;
};

struct BookOrbitBookmarkAckDeleted {
  int32_t serverId = -1;
  bool applied = false;
};

struct BookOrbitBookmarkAckBook {
  std::string hash;
  std::vector<BookOrbitBookmarkAckApplied> applied;
  std::vector<BookOrbitBookmarkAckDeleted> deleted;
};

struct BookOrbitBookmarkAckRequest {
  std::vector<BookOrbitBookmarkAckBook> books;
};
