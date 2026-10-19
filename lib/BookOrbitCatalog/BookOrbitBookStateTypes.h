#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Shared wire models for the KOReader plugin's match-check and book-state APIs.
// Kept in the catalog library so application code can use them without making
// the library depend on src/ firmware modules.
enum class BookOrbitMatchSource : uint8_t { CurrentFile, File, Statistics };

enum class BookOrbitBookStateStatus : uint8_t { Reading, Complete, Abandoned };


struct BookOrbitMatchCandidate {
  std::string hash;
  std::string title;
  std::string authors;
  uint32_t lastOpen = 0;
  BookOrbitMatchSource source = BookOrbitMatchSource::CurrentFile;
  int32_t bookFileId = -1;
  bool metadataAmbiguous = false;
};

struct BookOrbitMatch {
  std::string hash;
  int32_t bookId = -1;
  int32_t bookFileId = -1;
};

struct BookOrbitMatchCheckResponse {
  std::vector<BookOrbitMatch> matches;
  std::string libraryVersion;
};

// The catalog manifest's fileHash is the KOReader partial MD5 of the exact
// bytes served by that file's download URL; it is null for transformed files.
struct BookOrbitManifestFileHash {
  int32_t fileId = -1;
  std::string format;
  std::string hash;
};

struct BookOrbitBookStateUpload {
  std::string hash;
  bool hasStatus = false;
  BookOrbitBookStateStatus status = BookOrbitBookStateStatus::Reading;
  std::string statusModified;
  bool hasRating = false;
  uint8_t rating = 0;
  bool ratingCleared = false;
};

struct BookOrbitBookStateResult {
  std::string hash;
  bool unmatched = false;
  bool statusApplied = false;
  bool ratingApplied = false;
  bool reviewApplied = false;
  bool ratingSet = false;
  int8_t rating = -1;
  std::string ratingUpdatedAt;
  bool reviewNotePresent = false;
  bool reviewNoteSet = false;
  std::string reviewUpdatedAt;
};

// Receives decoded UTF-8 chunks of the response's reviewNote. `final` marks the
// end of this one note (including a null/empty server note, with len zero).
using BookOrbitReviewNoteSink = bool (*)(void* context, const uint8_t* bytes, size_t length, bool final);
