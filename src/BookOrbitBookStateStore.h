#pragma once

#include <BookOrbitCatalogClient.h>
#include <BookOrbitBookStateTypes.h>
#include <HalStorage.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Per-content-hash BookOrbit state and review cache. Queue records are one small
// file per hash; review text is a separate versioned SD file and is never kept in
// the store's resident state.
namespace BookOrbitBookStateStore {

constexpr size_t MAX_REVIEW_NOTE_BYTES = 40000;  // server allows 10,000 Unicode characters

struct State {
  // Cold-path cache state is bounded; queued fields use monotonic revisions.
  uint32_t nextRevision = 0;
  std::string hash;
  bool localStatusSet = false;
  BookOrbitCatalogClient::ReadStatus localStatus = BookOrbitCatalogClient::ReadStatus::Unread;
  bool canonicalStatusSet = false;
  BookOrbitCatalogClient::ReadStatus canonicalStatus = BookOrbitCatalogClient::ReadStatus::Unread;
  bool pendingStatus = false;
  BookOrbitBookStateStatus syncStatus = BookOrbitBookStateStatus::Reading;
  uint32_t statusRevision = 0;
  std::string statusModified;
  bool canonicalRatingSet = false;
  int8_t canonicalRating = -1;
  std::string ratingUpdatedAt;
  bool pendingRating = false;
  uint8_t rating = 0;
  bool ratingCleared = false;
  uint32_t ratingRevision = 0;
  std::string ratingModified;
  int32_t bookId = -1;
  int32_t bookFileId = -1;
  std::string libraryVersion;
  bool reviewNoteSet = false;
  uint32_t reviewLength = 0;
  uint32_t reviewFileGeneration = 0;
  uint32_t reviewCrc = 0;
  std::string reviewUpdatedAt;
};

enum class QueueResult : uint8_t { Queued, Skipped, Invalid, IoError };

struct ReviewNoteWriter {
  ReviewNoteWriter() = default;
  ~ReviewNoteWriter();
  ReviewNoteWriter(const ReviewNoteWriter&) = delete;
  ReviewNoteWriter& operator=(const ReviewNoteWriter&) = delete;

 private:
  friend bool beginReviewUpdate(const std::string&, const std::string&, ReviewNoteWriter&);
  friend bool writeReviewChunk(ReviewNoteWriter&, const uint8_t*, size_t);
  friend bool finishReviewUpdate(ReviewNoteWriter&, bool, const std::string&, State&);
  friend void abortReviewUpdate(ReviewNoteWriter&);

  HalFile file;
  std::array<char, 128> tempPath{};
  std::string hash;
  std::string updatedAt;
  uint32_t length = 0;
  uint32_t crcState = 0xFFFFFFFFu;
  bool active = false;
};

bool read(const std::string& hash, State& out);
QueueResult queueReaderOpened(const std::string& hash, const std::string& modifiedDay);
QueueResult queueFinished(const std::string& hash, const std::string& modifiedDay);
QueueResult queueStatus(const std::string& hash, BookOrbitCatalogClient::ReadStatus status,
                       const std::string& modifiedDay);
QueueResult queueRating(const std::string& hash, uint8_t stars, const std::string& modifiedDay);

// Select one durable hash record in lexical cursor order. includeCursor=false
// skips the exact hash rejected earlier in the current drain.
bool nextPending(const std::string& cursor, bool includeCursor, State& out);

bool rememberMapping(const std::string& hash, int32_t bookId, int32_t bookFileId,
                     const std::string& libraryVersion);
// Updates the canonical catalog value without changing newer local pending fields.
bool rememberCanonicalStatus(const std::string& hash, BookOrbitCatalogClient::ReadStatus status);
bool rememberCanonicalRating(const std::string& hash, bool isSet, int8_t rating,
                             const std::string& updatedAt);
// Clears only the exact in-flight field revisions; a concurrent newer edit stays queued.
bool acknowledge(const State& sent, bool acknowledgeStatus, bool acknowledgeRating);

bool beginReviewUpdate(const std::string& hash, const std::string& updatedAt, ReviewNoteWriter& writer);
bool writeReviewChunk(ReviewNoteWriter& writer, const uint8_t* bytes, size_t length);
// Stores a server-present note atomically; noteSet=false records a canonical server clear.
bool finishReviewUpdate(ReviewNoteWriter& writer, bool noteSet, const std::string& updatedAt, State& out);
void abortReviewUpdate(ReviewNoteWriter& writer);
// Reads one bounded window from the cached note, backing the end off an incomplete UTF-8 codepoint.
// `noteLength` comes from the cached State; `nextOffset` advances only over complete codepoints.
bool readReviewWindow(const std::string& hash, uint32_t offset, uint32_t noteLength, uint8_t* out,
                     size_t capacity, size_t& bytesRead, uint32_t& nextOffset);
bool readReviewChunk(const std::string& hash, uint32_t offset, uint8_t* out, size_t capacity,
                     size_t& bytesRead);
bool verifyReview(const std::string& hash);

}  // namespace BookOrbitBookStateStore
