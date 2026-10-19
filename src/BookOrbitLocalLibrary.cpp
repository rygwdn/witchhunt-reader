#include "BookOrbitLocalLibrary.h"

#include <HalStorage.h>
#include <LibraryIndexReader.h>
#include <LibraryKeys.h>
#include <algorithm>
#include <vector>

namespace BookOrbitLocalLibrary {
namespace {

bool readable(const std::string& path, const uint32_t expectedSize) {
  HalFile file;
  return Storage.openFileForRead("BOCat", path.c_str(), file) && file.size() > 0 &&
         (expectedSize == 0 || file.size() == expectedSize);
}

const char* filename(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

bool sameFilename(const char* left, const char* right) {
  const auto lower = [](const unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
  };
  while (*left && *right) {
    if (lower(static_cast<unsigned char>(*left++)) != lower(static_cast<unsigned char>(*right++))) return false;
  }
  return *left == *right;
}
bool filenameLess(const char* left, const char* right) {
  const auto lower = [](const unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
  };
  while (*left && *right) {
    const unsigned char a = lower(static_cast<unsigned char>(*left++));
    const unsigned char b = lower(static_cast<unsigned char>(*right++));
    if (a != b) return a < b;
  }
  return *left == '\0' && *right != '\0';
}

}  // namespace

std::string downloadedPath(const std::string& expectedPath, const uint32_t expectedSize, const char* indexPath) {
  if (expectedPath.empty()) return {};
  // A just-finished download is usable before the next New/Authors index build.
  if (readable(expectedPath, expectedSize)) return expectedPath;

  LibraryIndexReader index;
  if (!index.open(indexPath)) return {};
  const char* name = filename(expectedPath);
  std::string found;
  std::string candidate;
  const uint16_t bookCount = index.header().bookCount;
  uint32_t first = 0;
  uint32_t last = bookCount;
  if (expectedSize != 0) {
    const uint32_t expectedIdentity = LibraryKeys::bookIdentity(name, expectedSize);
    uint32_t low = 0;
    uint32_t high = bookCount;
    while (low < high) {
      const uint32_t middle = low + (high - low) / 2;
      library::BookRecord record{};
      if (!index.book(static_cast<uint16_t>(middle), record)) return {};
      if (record.identity < expectedIdentity)
        low = middle + 1;
      else
        high = middle;
    }
    first = low;
    low = first;
    high = bookCount;
    while (low < high) {
      const uint32_t middle = low + (high - low) / 2;
      library::BookRecord record{};
      if (!index.book(static_cast<uint16_t>(middle), record)) return {};
      if (record.identity <= expectedIdentity)
        low = middle + 1;
      else
        high = middle;
    }
    last = low;
  }
  for (uint32_t position = first; position < last; ++position) {
    library::BookRecord record{};
    if (!index.book(static_cast<uint16_t>(position), record)) return {};
    if (!index.blobString(record.pathOff, candidate)) return {};
    if (!sameFilename(name, filename(candidate))) continue;
    HalFile file;
    if (!Storage.openFileForRead("BOCat", candidate.c_str(), file) || file.size() == 0 ||
        (expectedSize != 0 && file.size() != expectedSize))
      continue;
    const auto size = static_cast<uint32_t>(file.size());
    if (record.identity != LibraryKeys::bookIdentity(name, size) ||
        record.identity != LibraryKeys::bookIdentity(filename(candidate), size))
      continue;
    // Older catalogs omit size. Distinct matching local copies must not silently choose a book.
    if (!found.empty()) return {};
    found = candidate;
  }
  return found;
}

void downloadedFlags(const std::vector<std::string>& expectedPaths, std::vector<uint8_t>& flags,
                    const char* indexPath) {
  flags.assign(expectedPaths.size(), 0);
  if (expectedPaths.size() > UINT32_MAX) return;
  std::vector<uint32_t> order;
  order.reserve(expectedPaths.size());
  for (uint32_t i = 0; i < expectedPaths.size(); ++i) {
    if (expectedPaths[i].empty()) continue;
    if (readable(expectedPaths[i], 0)) {
      flags[i] = 1;
      continue;
    }
    order.push_back(i);
  }
  if (order.empty()) return;
  std::sort(order.begin(), order.end(), [&](const uint32_t left, const uint32_t right) {
    return filenameLess(filename(expectedPaths[left]), filename(expectedPaths[right]));
  });

  LibraryIndexReader index;
  if (!index.open(indexPath)) return;
  auto clearIndexedFlags = [&]() {
    for (const uint32_t request : order)
      if (flags[request] != 1) flags[request] = 0;
  };
  std::string candidate;
  for (uint16_t position = 0; position < index.header().bookCount; ++position) {
    library::BookRecord record{};
    if (!index.book(position, record) || !index.blobString(record.pathOff, candidate)) {
      clearIndexedFlags();
      return;
    }
    const char* candidateName = filename(candidate);
    auto match = std::lower_bound(order.begin(), order.end(), candidateName,
                                  [&](const uint32_t request, const char* name) {
                                    return filenameLess(filename(expectedPaths[request]), name);
                                  });
    if (match == order.end() || !sameFilename(filename(expectedPaths[*match]), candidateName)) continue;
    HalFile file;
    if (!Storage.openFileForRead("BOCat", candidate.c_str(), file) || file.size() == 0) continue;
    const uint32_t size = static_cast<uint32_t>(file.size());
    if (record.identity != LibraryKeys::bookIdentity(candidateName, size)) continue;
    for (; match != order.end(); ++match) {
      const uint32_t request = *match;
      if (!sameFilename(filename(expectedPaths[request]), candidateName)) break;
      if (flags[request] == 1) continue;
      if (flags[request] == 0)
        flags[request] = 2;
      else
        flags[request] = 3;
    }
  }
  for (uint8_t& flag : flags) flag = (flag == 1 || flag == 2) ? 1 : 0;
}

}  // namespace BookOrbitLocalLibrary
