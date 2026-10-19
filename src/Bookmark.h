#pragma once

#include <cstdint>
#include <string>

struct Bookmark {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  std::string name;  // optional user-provided label (empty = use default)
  // Device identity and exact structural position are one packed string so local-only bookmarks
  // do not pay for several empty string objects. BookOrbitBookmarkSync owns its encoding.
  int32_t remoteServerId = -1;
  std::string syncMetadata;
};
