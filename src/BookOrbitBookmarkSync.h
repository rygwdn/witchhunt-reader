#pragma once

#include <Epub.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "BookmarkStore.h"
#include <BookOrbitBookmarkTypes.h>

namespace BookOrbitBookmarkSync {

struct PositionMetadataView {
  std::string_view datetime;
  std::string_view position;
};

struct ApplyResult {
  BookOrbitBookmarkAckBook ack;
  bool positionFailure = false;
};

// Create metadata only when a structural XPointer round-trips to an exact XHTML match.
std::string makeLocalPositionMetadata(const std::shared_ptr<Epub>& epub, int spineIndex, int page, int pageCount);
std::string encodePositionMetadata(std::string_view datetime, std::string_view position);
bool decodePositionMetadata(const std::string& encoded, PositionMetadataView& out);
std::string keyFor(std::string_view datetime, std::string_view position);

bool resolveExactPosition(const std::shared_ptr<Epub>& epub, const std::string& position, int& spineIndex,
                          float& intraSpineProgress, uint16_t* paragraphIndex = nullptr,
                          uint16_t* listItemIndex = nullptr);
// Resolve a stored structural position to reader-local coordinates; the optional page comes from
// the local section layout/structural-index LUT, never from the server's page hint.
bool resolvePositionForNavigation(const std::shared_ptr<Epub>& epub, const std::string& cachePath,
                                 const std::string& position, int& spineIndex, float& intraSpineProgress,
                                 uint16_t& pageNumber, uint16_t* paragraphIndex = nullptr,
                                 uint16_t* listItemIndex = nullptr);
// Build one server-sized changes batch and its device key set. The store is durably saved before
// the snapshot can be sent; revision is the ownership guard for the later main-thread apply.
bool prepareExchange(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const std::string& hash,
                     size_t changeOffset, BookOrbitBookmarkExchangeBook& request, size_t& totalChanges,
                     uint32_t& revision);

// Apply only exact structural positions, persist before returning an ack, and update the global
// bookmark index after the per-book store is durable.
bool applyResponse(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const std::string& sourcePath,
                   const std::string& cachePath, const std::string& bookTitle, uint32_t expectedRevision,
                   const BookOrbitBookmarkExchangeResult& response, ApplyResult& out);

}  // namespace BookOrbitBookmarkSync
