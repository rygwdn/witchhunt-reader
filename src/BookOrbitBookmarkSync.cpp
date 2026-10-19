#include "BookOrbitBookmarkSync.h"

#include <HalClock.h>
#include <MD5Builder.h>
#include <cmath>

#include <algorithm>
#include <ctime>
#include <memory>
#include <optional>
#include <string>

#include "ChapterXPathIndexer.h"
#include "GlobalBookmarkIndex.h"
#include "ProgressMapper.h"
#include <Epub/Section.h>

namespace {
constexpr size_t MAX_KEYS_PER_BOOK = 500;
constexpr size_t MAX_CHANGES_PER_REQUEST = 8;

std::string currentDeviceDatetime() {
  if (!HalClock::isSynced()) return {};
  const time_t now = HalClock::now();
  struct tm local{};
  if (now <= 0 || !localtime_r(&now, &local)) return {};
  char datetime[20];
  if (strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S", &local) != 19) return {};
  return std::string(datetime);
}

bool exactPosition(const std::shared_ptr<Epub>& epub, const std::string& position, int& spineIndex,
                   float& intraSpineProgress, uint16_t* paragraphIndex = nullptr, uint16_t* listItemIndex = nullptr) {
  if (paragraphIndex != nullptr) *paragraphIndex = 0;
  if (listItemIndex != nullptr) *listItemIndex = 0;
  if (!epub || !ChapterXPathIndexer::tryExtractSpineIndexFromXPath(position, spineIndex) || spineIndex < 0 ||
      spineIndex >= epub->getSpineItemsCount()) {
    return false;
  }
  bool exactMatch = false;
  uint16_t resolvedListItem = 0;
  if (!ChapterXPathIndexer::findProgressForXPath(epub, spineIndex, position, intraSpineProgress, exactMatch,
                                                  &resolvedListItem) ||
      !exactMatch) {
    return false;
  }
  if (paragraphIndex != nullptr) {
    ChapterXPathIndexer::tryExtractParagraphIndexFromXPath(position, *paragraphIndex);
  }
  if (listItemIndex != nullptr) *listItemIndex = resolvedListItem;
  return true;
}

std::string legacyBookmarkPosition(const Bookmark& bookmark, const std::shared_ptr<Epub>& epub,
                                   const std::string& cachePath) {
  Section::CachedPageInfo pageInfo;
  if (!epub || !Section::getCachedPageInfoFromAnyLayout(cachePath, bookmark.spineIndex, bookmark.pageNumber,
                                                         pageInfo) ||
      pageInfo.pageCount == 0) {
    return {};
  }

  std::string position;
  if (pageInfo.visibleTextOffset) {
    position = ChapterXPathIndexer::findXPathForVisibleOffset(epub, bookmark.spineIndex, *pageInfo.visibleTextOffset);
  }
  int resolvedSpine = -1;
  float progress = 0.0f;
  if (position.empty() || !exactPosition(epub, position, resolvedSpine, progress) ||
      resolvedSpine != bookmark.spineIndex) {
    CrossPointPosition local{};
    local.spineIndex = bookmark.spineIndex;
    local.pageNumber = bookmark.pageNumber;
    local.totalPages = pageInfo.pageCount;
    position = ProgressMapper::toKOReader(epub, local).xpath;
  }
  if (position.empty() || !exactPosition(epub, position, resolvedSpine, progress) ||
      resolvedSpine != bookmark.spineIndex) {
    return {};
  }
  return BookOrbitBookmarkSync::encodePositionMetadata({}, position);
}

std::optional<uint16_t> localPageForPosition(const std::string& cachePath, const int spineIndex,
                                             const float intraSpineProgress, const uint16_t paragraphIndex,
                                             const uint16_t listItemIndex) {
  if (paragraphIndex != 0) {
    if (const auto page = Section::getPageForIndexFromAnyLayout(cachePath, spineIndex, paragraphIndex, false)) {
      return page;
    }
  }
  if (listItemIndex != 0) {
    if (const auto page = Section::getPageForIndexFromAnyLayout(cachePath, spineIndex, listItemIndex, true)) {
      return page;
    }
  }

  Section::CachedPageInfo pageInfo;
  if (!Section::getCachedPageInfoFromAnyLayout(cachePath, spineIndex, 0, pageInfo) || pageInfo.pageCount == 0) {
    return std::nullopt;
  }
  const float clamped = std::clamp(intraSpineProgress, 0.0f, 1.0f);
  const auto lastPage = static_cast<int>(pageInfo.pageCount) - 1;
  return static_cast<uint16_t>(std::clamp(static_cast<int>(std::lround(clamped * lastPage)), 0, lastPage));
}

}  // namespace

namespace BookOrbitBookmarkSync {
bool resolveExactPosition(const std::shared_ptr<Epub>& epub, const std::string& position, int& spineIndex,
                          float& intraSpineProgress, uint16_t* paragraphIndex, uint16_t* listItemIndex) {
  return exactPosition(epub, position, spineIndex, intraSpineProgress, paragraphIndex, listItemIndex);
}

bool resolvePositionForNavigation(const std::shared_ptr<Epub>& epub, const std::string& cachePath,
                                 const std::string& position, int& spineIndex, float& intraSpineProgress,
                                 uint16_t& pageNumber, uint16_t* paragraphIndex, uint16_t* listItemIndex) {
  uint16_t paragraph = 0;
  uint16_t listItem = 0;
  if (!exactPosition(epub, position, spineIndex, intraSpineProgress, &paragraph, &listItem)) return false;
  if (paragraphIndex != nullptr) *paragraphIndex = paragraph;
  if (listItemIndex != nullptr) *listItemIndex = listItem;
  if (const auto localPage = localPageForPosition(cachePath, spineIndex, intraSpineProgress, paragraph, listItem)) {
    pageNumber = *localPage;
  }
  return true;
}


std::string encodePositionMetadata(const std::string_view datetime, const std::string_view position) {
  if ((!datetime.empty() && datetime.size() != 19) || position.empty() || position.size() > 4000) return {};
  std::string encoded;
  encoded.reserve(datetime.size() + position.size() + 1);
  encoded.append(datetime);
  encoded.push_back('|');
  encoded.append(position);
  if (encoded.size() > BookmarkStore::MAX_SYNC_METADATA_LENGTH) return {};
  return encoded;
}

bool decodePositionMetadata(const std::string& encoded, PositionMetadataView& out) {
  const size_t separator = encoded.find('|');
  if (separator == std::string::npos || (separator != 0 && separator != 19) || separator + 1 >= encoded.size() ||
      encoded.size() > BookmarkStore::MAX_SYNC_METADATA_LENGTH) {
    return false;
  }
  out.datetime = std::string_view(encoded.data(), separator);
  out.position = std::string_view(encoded.data() + separator + 1, encoded.size() - separator - 1);
  return out.position.size() <= 4000;
}

std::string keyFor(const std::string_view datetime, const std::string_view position) {
  if (datetime.size() != 19 || position.empty()) return {};
  std::string material;
  material.reserve(datetime.size() + position.size() + 1);
  material.append(datetime);
  material.push_back('|');
  material.append(position);
  MD5Builder md5;
  md5.begin();
  md5.add(material.c_str());
  md5.calculate();
  const String digest = md5.toString();
  return std::string(digest.c_str());
}

std::string makeLocalPositionMetadata(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                     const int pageCount) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount() || page < 0 || pageCount <= 0) return {};
  CrossPointPosition local{};
  local.spineIndex = spineIndex;
  local.pageNumber = page;
  local.totalPages = pageCount;
  const KOReaderPosition mapped = ProgressMapper::toKOReader(epub, local);
  int exactSpine = -1;
  float intraSpineProgress = 0.0f;
  if (!exactPosition(epub, mapped.xpath, exactSpine, intraSpineProgress) || exactSpine != spineIndex) {
    LOG_DBG("BOBkm", "Skipping sync metadata: page did not map to an exact structural XPointer");
    return {};
  }
  return encodePositionMetadata(currentDeviceDatetime(), mapped.xpath);
}

bool prepareExchange(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const std::string& hash,
                     const size_t changeOffset, BookOrbitBookmarkExchangeBook& request, size_t& totalChanges,
                     uint32_t& revision) {
  if (!epub || hash.size() != 32 || !store.isLoadValid()) return false;
  request = {};
  request.hash = hash;
  const auto& bookmarks = store.getAll();
  request.keys.reserve(std::min(bookmarks.size(), MAX_KEYS_PER_BOOK));
  request.changes.reserve(MAX_CHANGES_PER_REQUEST);
  bool keysComplete = bookmarks.size() <= MAX_KEYS_PER_BOOK;
  totalChanges = 0;
  bool storeChanged = false;
  const std::string now = currentDeviceDatetime();
  if (now.empty()) return false;

  for (size_t index = 0; index < bookmarks.size(); ++index) {
    const Bookmark& bookmark = bookmarks[index];
    if (bookmark.syncMetadata.empty()) {
      const std::string migrated = legacyBookmarkPosition(bookmark, epub, epub->getCachePath());
      if (migrated.empty() || !store.setSyncMetadataAt(index, migrated)) {
        keysComplete = false;
        continue;
      }
      storeChanged = true;
    }
    PositionMetadataView metadata;
    if (!decodePositionMetadata(bookmark.syncMetadata, metadata)) {
      keysComplete = false;
      continue;
    }
    if (metadata.datetime.empty()) {
      const std::string stamped = encodePositionMetadata(now, metadata.position);
      if (stamped.empty() || !store.setSyncMetadataAt(index, stamped)) return false;
      storeChanged = true;
      PositionMetadataView updated;
      if (!decodePositionMetadata(store.getAll()[index].syncMetadata, updated)) return false;
      metadata = updated;
    }
    if (metadata.datetime.size() != 19) {
      keysComplete = false;
      continue;
    }

    const std::string key = keyFor(metadata.datetime, metadata.position);
    if (key.empty()) {
      keysComplete = false;
      continue;
    }
    if (request.keys.size() < MAX_KEYS_PER_BOOK) {
      BookOrbitBookmarkKey encoded;
      std::copy_n(key.data(), encoded.key.size() - 1, encoded.key.data());
      std::copy_n(metadata.datetime.data(), encoded.datetime.size() - 1, encoded.datetime.data());
      request.keys.push_back(encoded);
    } else {
      keysComplete = false;
    }

    if (totalChanges >= changeOffset && request.changes.size() < MAX_CHANGES_PER_REQUEST) {
      BookOrbitBookmarkChange change;
      change.datetime = std::string(metadata.datetime);
      change.position = std::string(metadata.position);
      change.page = bookmark.pageNumber == BookmarkStore::UNKNOWN_PAGE_NUMBER ? 0 : bookmark.pageNumber;
      const int tocIndex = epub->getTocIndexForSpineIndex(bookmark.spineIndex);
      if (tocIndex >= 0) change.chapter = epub->getTocItem(tocIndex).title;
      change.note = bookmark.name;
      request.changes.push_back(std::move(change));
    }
    ++totalChanges;
  }

  request.keysComplete = keysComplete;
  if (!store.save()) return false;
  revision = store.revision();
  if (storeChanged) {
    GLOBAL_BOOKMARKS.syncFromStore(store, epub->getPath(), epub->getCachePath(), epub->getTitle(), false);
  }
  return true;
}

bool applyResponse(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const std::string& sourcePath,
                   const std::string& cachePath, const std::string& bookTitle, const uint32_t expectedRevision,
                   const BookOrbitBookmarkExchangeResult& response, ApplyResult& out) {
  if (!epub || store.revision() != expectedRevision) return false;
  out = {};
  out.ack.hash = response.hash;
  out.ack.applied.reserve(response.add.size());
  out.ack.deleted.reserve(response.remove.size());

  for (const BookOrbitBookmarkAdd& entry : response.add) {
    int spineIndex = -1;
    float intraSpineProgress = 0.0f;
    uint16_t paragraphIndex = 0;
    uint16_t listItemIndex = 0;
    if (entry.serverId <= 0 ||
        !exactPosition(epub, entry.position, spineIndex, intraSpineProgress, &paragraphIndex, &listItemIndex)) {
      out.ack.applied.push_back({entry.serverId, false, {}, {}, {}});
      out.positionFailure = true;
      continue;
    }
    const auto localPage = localPageForPosition(cachePath, spineIndex, intraSpineProgress, paragraphIndex,
                                                listItemIndex);

    int existingIndex = -1;
    const auto& current = store.getAll();
    for (size_t index = 0; index < current.size(); ++index) {
      if (current[index].remoteServerId == entry.serverId) {
        existingIndex = static_cast<int>(index);
        break;
      }
      PositionMetadataView metadata;
      if (decodePositionMetadata(current[index].syncMetadata, metadata) && metadata.position == entry.position) {
        existingIndex = static_cast<int>(index);
        break;
      }
    }

    std::string datetime;
    if (existingIndex >= 0) {
      const Bookmark& existing = current[static_cast<size_t>(existingIndex)];
      PositionMetadataView metadata;
      if (!decodePositionMetadata(existing.syncMetadata, metadata) || metadata.datetime.size() != 19) {
        out.ack.applied.push_back({entry.serverId, false, {}, {}, {}});
        out.positionFailure = true;
        continue;
      }
      datetime = std::string(metadata.datetime);
      store.setRemoteServerIdAt(static_cast<size_t>(existingIndex), entry.serverId);
      const uint16_t resolvedPage = localPage.value_or(BookmarkStore::UNKNOWN_PAGE_NUMBER);
      if (existing.pageNumber != resolvedPage) {
        store.setPageNumberAt(static_cast<size_t>(existingIndex), resolvedPage);
      }
    } else {
      datetime = currentDeviceDatetime();
      if (datetime.size() != 19) {
        out.ack.applied.push_back({entry.serverId, false, {}, {}, {}});
        continue;
      }
      Bookmark bookmark;
      bookmark.spineIndex = static_cast<uint16_t>(spineIndex);
      bookmark.pageNumber = localPage.value_or(BookmarkStore::UNKNOWN_PAGE_NUMBER);
      bookmark.name = entry.title;
      bookmark.remoteServerId = entry.serverId;
      bookmark.syncMetadata = encodePositionMetadata(datetime, entry.position);
      if (bookmark.syncMetadata.empty() || !store.applyRemoteAdd(std::move(bookmark))) {
        out.ack.applied.push_back({entry.serverId, false, {}, {}, {}});
        continue;
      }
    }

    const std::string key = keyFor(datetime, entry.position);
    if (key.empty()) {
      out.ack.applied.push_back({entry.serverId, false, {}, {}, {}});
      out.positionFailure = true;
      continue;
    }
    out.ack.applied.push_back({entry.serverId, true, key, datetime, entry.position});
  }

  for (const BookOrbitBookmarkDelete& entry : response.remove) {
    int existingIndex = -1;
    const auto& current = store.getAll();
    for (size_t index = 0; index < current.size(); ++index) {
      const Bookmark& bookmark = current[index];
      if (bookmark.remoteServerId == entry.serverId) {
        existingIndex = static_cast<int>(index);
        break;
      }
      PositionMetadataView metadata;
      if (decodePositionMetadata(bookmark.syncMetadata, metadata) && metadata.datetime.size() == 19 &&
          keyFor(metadata.datetime, metadata.position) == entry.key) {
        existingIndex = static_cast<int>(index);
        break;
      }
    }
    if (existingIndex >= 0) store.removeAt(static_cast<size_t>(existingIndex));
    out.ack.deleted.push_back({entry.serverId, true});
  }

  if (!store.save()) return false;
  GLOBAL_BOOKMARKS.syncFromStore(store, sourcePath, cachePath, bookTitle, false);
  return true;
}

}  // namespace BookOrbitBookmarkSync
