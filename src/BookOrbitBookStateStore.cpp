#include "BookOrbitBookStateStore.h"

#include <Arduino.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "ReadingStatsSlotFile.h"

namespace {
constexpr char ROOT[] = "/.crosspoint/bookorbit-book-state";
constexpr char PENDING_DIR[] = "/.crosspoint/bookorbit-book-state/pending";
constexpr char CACHE_DIR[] = "/.crosspoint/bookorbit-book-state/cache";
constexpr char REVIEW_DIR[] = "/.crosspoint/bookorbit-book-state/reviews";
constexpr uint8_t STATE_MAGIC[] = {'B', 'O', 'S', 1};
constexpr size_t STATE_BYTES = 228;
constexpr size_t STATE_CRC_OFFSET = 224;
constexpr size_t NOTE_HEADER_BYTES = 80;
constexpr uint8_t NOTE_MAGIC[] = {'B', 'O', 'R', 1};
constexpr size_t MAX_REVIEW_BYTES = BookOrbitBookStateStore::MAX_REVIEW_NOTE_BYTES;

constexpr size_t FLAGS_AT = 5;
constexpr uint8_t LOCAL_STATUS_SET = 1u << 0;
constexpr uint8_t CANONICAL_STATUS_SET = 1u << 1;
constexpr uint8_t PENDING_STATUS = 1u << 2;
constexpr uint8_t CANONICAL_RATING_SET = 1u << 3;
constexpr uint8_t PENDING_RATING = 1u << 4;
constexpr uint8_t REVIEW_NOTE_SET = 1u << 5;
constexpr uint8_t PENDING_RATING_CLEARED = 1u << 6;

SemaphoreHandle_t stateMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}
SemaphoreHandle_t reviewMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}
// State and wire-image scratch are bounded and serialized by stateMutex(), keeping
// the pending overlay and record image out of nested caller stack frames.
BookOrbitBookStateStore::State pendingScratch;
std::array<uint8_t, STATE_BYTES> recordScratch{};

class StateLock {
 public:
  StateLock() : mutex(stateMutex()), locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
  ~StateLock() { if (locked) xSemaphoreGive(mutex); }
  bool acquired() const { return locked; }
 private:
  SemaphoreHandle_t mutex;
  bool locked;
};

void put32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}
uint32_t get32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}
void putI32(uint8_t* out, int32_t value) { put32(out, static_cast<uint32_t>(value)); }
int32_t getI32(const uint8_t* in) { return static_cast<int32_t>(get32(in)); }

uint32_t updateCrc(uint32_t crc, const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

bool validDay(const std::string& day) {
  if (day.empty()) return true;
  if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
  for (size_t i = 0; i < day.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (day[i] < '0' || day[i] > '9') return false;
  }
  return true;
}

void copyFixed(char* out, size_t capacity, const std::string& value) {
  memset(out, 0, capacity);
  const size_t count = std::min(capacity - 1, value.size());
  if (count) memcpy(out, value.data(), count);
}
std::string fixedString(const uint8_t* bytes, size_t capacity) {
  const char* chars = reinterpret_cast<const char*>(bytes);
  size_t length = 0;
  while (length < capacity && chars[length] != '\0') ++length;
  return std::string(chars, length);
}

bool normalizeHash(const std::string& hash, std::string& normalized) {
  ReadingStatsSlotFile::DocKey key{};
  if (!ReadingStatsSlotFile::parseDocId(hash, key)) return false;
  normalized = ReadingStatsSlotFile::formatDocId(key);
  return true;
}

bool makePaths(const std::string& hash, std::string& pending, std::string& cache) {
  std::string normalized;
  if (!normalizeHash(hash, normalized)) return false;
  pending = std::string(PENDING_DIR) + "/" + normalized + ".state";
  cache = std::string(CACHE_DIR) + "/" + normalized + ".state";
  return true;
}

void encodeState(const BookOrbitBookStateStore::State& state, std::array<uint8_t, STATE_BYTES>& image) {
  image.fill(0);
  memcpy(image.data(), STATE_MAGIC, sizeof(STATE_MAGIC));
  uint8_t flags = 0;
  if (state.localStatusSet) flags |= LOCAL_STATUS_SET;
  if (state.canonicalStatusSet) flags |= CANONICAL_STATUS_SET;
  if (state.pendingStatus) flags |= PENDING_STATUS;
  if (state.canonicalRatingSet) flags |= CANONICAL_RATING_SET;
  if (state.pendingRating) flags |= PENDING_RATING;
  if (state.reviewNoteSet) flags |= REVIEW_NOTE_SET;
  if (state.ratingCleared) flags |= PENDING_RATING_CLEARED;
  image[4] = 1;
  image[FLAGS_AT] = flags;
  image[6] = static_cast<uint8_t>(state.canonicalStatus);
  image[7] = static_cast<uint8_t>(state.localStatus);
  image[8] = static_cast<uint8_t>(state.syncStatus);
  image[9] = state.rating;
  image[10] = static_cast<uint8_t>(state.canonicalRating);
  put32(image.data() + 12, state.nextRevision);
  put32(image.data() + 16, state.statusRevision);
  put32(image.data() + 20, state.ratingRevision);
  putI32(image.data() + 24, state.bookId);
  putI32(image.data() + 28, state.bookFileId);
  put32(image.data() + 32, state.reviewLength);
  put32(image.data() + 36, state.reviewCrc);
  copyFixed(reinterpret_cast<char*>(image.data() + 40), 11, state.statusModified);
  copyFixed(reinterpret_cast<char*>(image.data() + 51), 11, state.ratingModified);
  copyFixed(reinterpret_cast<char*>(image.data() + 62), 32, state.ratingUpdatedAt);
  copyFixed(reinterpret_cast<char*>(image.data() + 94), 32, state.libraryVersion);
  copyFixed(reinterpret_cast<char*>(image.data() + 126), 32, state.reviewUpdatedAt);
  copyFixed(reinterpret_cast<char*>(image.data() + 158), 33, state.hash);
  put32(image.data() + 218, state.reviewFileGeneration);
  put32(image.data() + STATE_CRC_OFFSET,
        ReadingStatsSlotFile::crc32(image.data(), STATE_CRC_OFFSET));
}

bool decodeState(const uint8_t* image, BookOrbitBookStateStore::State& state) {
  if (memcmp(image, STATE_MAGIC, sizeof(STATE_MAGIC)) != 0 || image[4] != 1 ||
      get32(image + STATE_CRC_OFFSET) != ReadingStatsSlotFile::crc32(image, STATE_CRC_OFFSET)) return false;
  const uint8_t flags = image[FLAGS_AT];
  if (image[6] > static_cast<uint8_t>(BookOrbitCatalogClient::ReadStatus::Abandoned) ||
      image[7] > static_cast<uint8_t>(BookOrbitCatalogClient::ReadStatus::Abandoned) ||
      image[8] > static_cast<uint8_t>(BookOrbitBookStateStatus::Abandoned) || image[9] > 5) return false;
  state = {};
  state.hash = fixedString(image + 158, 33);
  std::string normalized;
  if (!normalizeHash(state.hash, normalized) || normalized != state.hash) return false;
  state.localStatusSet = (flags & LOCAL_STATUS_SET) != 0;
  state.canonicalStatusSet = (flags & CANONICAL_STATUS_SET) != 0;
  state.pendingStatus = (flags & PENDING_STATUS) != 0;
  state.canonicalRatingSet = (flags & CANONICAL_RATING_SET) != 0;
  state.pendingRating = (flags & PENDING_RATING) != 0;
  state.reviewNoteSet = (flags & REVIEW_NOTE_SET) != 0;
  state.ratingCleared = (flags & PENDING_RATING_CLEARED) != 0;
  state.canonicalStatus = static_cast<BookOrbitCatalogClient::ReadStatus>(image[6]);
  state.localStatus = static_cast<BookOrbitCatalogClient::ReadStatus>(image[7]);
  state.syncStatus = static_cast<BookOrbitBookStateStatus>(image[8]);
  state.rating = image[9];
  state.canonicalRating = static_cast<int8_t>(image[10]);
  state.nextRevision = get32(image + 12);
  state.statusRevision = get32(image + 16);
  state.ratingRevision = get32(image + 20);
  state.bookId = getI32(image + 24);
  state.bookFileId = getI32(image + 28);
  state.reviewLength = get32(image + 32);
  state.reviewCrc = get32(image + 36);
  state.statusModified = fixedString(image + 40, 11);
  state.ratingModified = fixedString(image + 51, 11);
  state.ratingUpdatedAt = fixedString(image + 62, 32);
  state.libraryVersion = fixedString(image + 94, 32);
  state.reviewUpdatedAt = fixedString(image + 126, 32);
  state.reviewFileGeneration = get32(image + 218);
  return state.reviewLength <= MAX_REVIEW_BYTES;
}

bool readRecord(const std::string& path, BookOrbitBookStateStore::State& out) {
  HalFile file;
  if (!Storage.openFileForRead("BOState", path, file) || file.size64() != STATE_BYTES) return false;
  return file.read(recordScratch.data(), recordScratch.size()) == recordScratch.size() &&
         decodeState(recordScratch.data(), out);
}

bool recoverRecordPath(const std::string& path, const std::string& expectedHash) {
  const std::string tempPath = path + ".tmp";
  const std::string backupPath = path + ".bak";
  const bool haveTemp = Storage.exists(tempPath.c_str());
  const bool haveBackup = Storage.exists(backupPath.c_str());
  if (!haveTemp && !haveBackup) return true;

  const bool haveLive = Storage.exists(path.c_str());
  const bool liveValid = haveLive && readRecord(path, pendingScratch) && pendingScratch.hash == expectedHash;
  if (liveValid) {
    if ((haveBackup && !Storage.remove(backupPath.c_str())) ||
        (haveTemp && !Storage.remove(tempPath.c_str()))) return false;
    return true;
  }

  if (haveBackup && readRecord(backupPath, pendingScratch) && pendingScratch.hash == expectedHash) {
    if ((haveLive && !Storage.remove(path.c_str())) || !Storage.rename(backupPath.c_str(), path.c_str())) return false;
    if (haveTemp && !Storage.remove(tempPath.c_str())) return false;
    return true;
  }

  if (!haveLive && !haveBackup && haveTemp) return Storage.remove(tempPath.c_str());
  return false;
}

bool writeRecordAtomic(const std::string& path, const BookOrbitBookStateStore::State& state) {
  if (!recoverRecordPath(path, state.hash)) return false;
  const std::string tempPath = path + ".tmp";
  const std::string backupPath = path + ".bak";
  encodeState(state, recordScratch);
  bool written = false;
  {
    HalFile file;
    if (Storage.openFileForWrite("BOState", tempPath, file)) {
      written = file.write(recordScratch.data(), recordScratch.size()) == recordScratch.size();
      file.flush();
    }
  }
  if (!written || !readRecord(tempPath, pendingScratch) || pendingScratch.hash != state.hash) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  if (!Storage.publishStagedFileWithBackup(tempPath.c_str(), path.c_str(), backupPath.c_str())) {
    recoverRecordPath(path, state.hash);
    return false;
  }
  if (!readRecord(path, pendingScratch) || pendingScratch.hash != state.hash) {
    recoverRecordPath(path, state.hash);
    return false;
  }
  return !Storage.exists(backupPath.c_str()) || Storage.remove(backupPath.c_str());
}

bool loadLocked(const std::string& hash, BookOrbitBookStateStore::State& out) {
  std::string pendingPath, cachePath;
  if (!makePaths(hash, pendingPath, cachePath)) return false;
  std::string normalized;
  if (!normalizeHash(hash, normalized)) return false;
  if (!recoverRecordPath(cachePath, normalized) || !recoverRecordPath(pendingPath, normalized)) return false;
  out = {};
  out.hash = normalized;
  const bool haveCache = Storage.exists(cachePath.c_str());
  if (haveCache && (!readRecord(cachePath, out) || out.hash != normalized)) return false;
  if (Storage.exists(pendingPath.c_str())) {
    if (!readRecord(pendingPath, pendingScratch) || pendingScratch.hash != normalized) return false;
    if (!haveCache) {
      out = std::move(pendingScratch);
      return true;
    }
    out.localStatusSet = pendingScratch.localStatusSet;
    out.localStatus = pendingScratch.localStatus;
    out.pendingStatus = pendingScratch.pendingStatus;
    out.syncStatus = pendingScratch.syncStatus;
    out.statusRevision = pendingScratch.statusRevision;
    out.statusModified = pendingScratch.statusModified;
    out.pendingRating = pendingScratch.pendingRating;
    out.rating = pendingScratch.rating;
    out.ratingCleared = pendingScratch.ratingCleared;
    out.ratingRevision = pendingScratch.ratingRevision;
    out.ratingModified = pendingScratch.ratingModified;
    out.nextRevision = std::max(out.nextRevision, pendingScratch.nextRevision);
    if (out.bookId <= 0) out.bookId = pendingScratch.bookId;
    if (out.bookFileId <= 0) out.bookFileId = pendingScratch.bookFileId;
    if (out.libraryVersion.empty()) out.libraryVersion = pendingScratch.libraryVersion;
  }
  return true;
}

bool writeCacheLocked(const BookOrbitBookStateStore::State& state) {
  std::string pendingPath, cachePath;
  return makePaths(state.hash, pendingPath, cachePath) && Storage.ensureDirectoryExists(ROOT) &&
         Storage.ensureDirectoryExists(CACHE_DIR) && writeRecordAtomic(cachePath, state);
}

bool pendingPathFor(const std::string& hash, std::string& path) {
  std::string cachePath;
  return makePaths(hash, path, cachePath);
}

bool writePendingLocked(const BookOrbitBookStateStore::State& state) {
  std::string path;
  if (!pendingPathFor(state.hash, path) || !Storage.ensureDirectoryExists(ROOT) ||
      !Storage.ensureDirectoryExists(PENDING_DIR)) return false;
  if (!state.pendingStatus && !state.pendingRating) {
    if (!Storage.exists(path.c_str())) return true;
    return Storage.remove(path.c_str());
  }
  return writeRecordAtomic(path, state);
}


uint32_t nextRevision(BookOrbitBookStateStore::State& state) {
  ++state.nextRevision;
  if (state.nextRevision == 0) ++state.nextRevision;
  return state.nextRevision;
}

bool statusToWire(BookOrbitCatalogClient::ReadStatus status, BookOrbitBookStateStatus& out) {
  switch (status) {
    case BookOrbitCatalogClient::ReadStatus::Reading:
      out = BookOrbitBookStateStatus::Reading;
      return true;
    case BookOrbitCatalogClient::ReadStatus::Read:
      out = BookOrbitBookStateStatus::Complete;
      return true;
    case BookOrbitCatalogClient::ReadStatus::Abandoned:
      out = BookOrbitBookStateStatus::Abandoned;
      return true;
    default:
      return false;
  }
}

BookOrbitBookStateStore::QueueResult queueStatusLocked(const std::string& hash,
                                                        BookOrbitCatalogClient::ReadStatus status,
                                                        const std::string& modifiedDay,
                                                        const bool readerOpen) {
  if (!validDay(modifiedDay)) return BookOrbitBookStateStore::QueueResult::Invalid;
  BookOrbitBookStateStatus wireStatus;
  if (!statusToWire(status, wireStatus)) return BookOrbitBookStateStore::QueueResult::Invalid;
  BookOrbitBookStateStore::State state;
  if (!loadLocked(hash, state)) return BookOrbitBookStateStore::QueueResult::IoError;
  if (readerOpen) {
    if (state.localStatusSet) return BookOrbitBookStateStore::QueueResult::Skipped;
    if (state.canonicalStatusSet &&
        (state.canonicalStatus == BookOrbitCatalogClient::ReadStatus::Read ||
         state.canonicalStatus == BookOrbitCatalogClient::ReadStatus::Abandoned)) {
      return BookOrbitBookStateStore::QueueResult::Skipped;
    }
  }
  if (state.pendingStatus && state.syncStatus == wireStatus && state.statusModified == modifiedDay) {
    return BookOrbitBookStateStore::QueueResult::Queued;
  }
  state.localStatusSet = true;
  state.localStatus = status;
  state.pendingStatus = true;
  state.syncStatus = wireStatus;
  state.statusModified = modifiedDay;
  state.statusRevision = nextRevision(state);
  if (!writePendingLocked(state)) return BookOrbitBookStateStore::QueueResult::IoError;
  if (!writeCacheLocked(state)) LOG_ERR("BOState", "Pending status saved but cache write failed");
  return BookOrbitBookStateStore::QueueResult::Queued;
}

void makeNotePath(const std::string& hash, const std::string& updatedAt, const uint32_t crc,
                  const uint32_t generation, std::array<char, 128>& path) {
  char stamp[24]{};
  size_t used = 0;
  for (const char ch : updatedAt) {
    if (ch >= '0' && ch <= '9' && used + 1 < sizeof(stamp)) stamp[used++] = ch;
  }
  if (used == 0) memcpy(stamp, "unknown", 7);
  snprintf(path.data(), path.size(), "%s/%s-%s-%08x-%08x.note", REVIEW_DIR, hash.c_str(), stamp, crc, generation);
}

void encodeNoteHeader(uint8_t* out, const std::string& hash, const std::string& updatedAt,
                      const uint32_t length, const uint32_t crc) {
  memset(out, 0, NOTE_HEADER_BYTES);
  memcpy(out, NOTE_MAGIC, sizeof(NOTE_MAGIC));
  out[4] = 1;
  put32(out + 8, length);
  put32(out + 12, crc);
  memcpy(out + 16, hash.data(), 32);
  copyFixed(reinterpret_cast<char*>(out + 48), 27, updatedAt);
  put32(out + 76, ReadingStatsSlotFile::crc32(out, 76));
}

bool verifyNoteHeader(HalFile& file, const BookOrbitBookStateStore::State& state) {
  if (!state.reviewNoteSet || state.reviewLength > MAX_REVIEW_BYTES) return false;
  std::array<uint8_t, NOTE_HEADER_BYTES> header{};
  if (!file.seekSet64(0) || file.read(header.data(), header.size()) != header.size()) return false;
  return memcmp(header.data(), NOTE_MAGIC, sizeof(NOTE_MAGIC)) == 0 && header[4] == 1 &&
         get32(header.data() + 8) == state.reviewLength && get32(header.data() + 12) == state.reviewCrc &&
         fixedString(header.data() + 16, 32) == state.hash && fixedString(header.data() + 48, 27) == state.reviewUpdatedAt &&
         get32(header.data() + 76) == ReadingStatsSlotFile::crc32(header.data(), 76) &&
         file.size64() == NOTE_HEADER_BYTES + state.reviewLength;
}

std::string notePath(const BookOrbitBookStateStore::State& state) {
  std::array<char, 128> path{};
  makeNotePath(state.hash, state.reviewUpdatedAt, state.reviewCrc, state.reviewFileGeneration, path);
  return path.data();
}

bool newerOrEqual(const std::string& incoming, const std::string& current) {
  if (current.empty()) return true;
  if (incoming.empty()) return false;
  return incoming >= current;
}

}  // namespace

namespace BookOrbitBookStateStore {

ReviewNoteWriter::~ReviewNoteWriter() { abortReviewUpdate(*this); }

bool read(const std::string& hash, State& out) {
  std::string normalized;
  if (!normalizeHash(hash, normalized)) return false;
  const StateLock lock;
  return lock.acquired() && loadLocked(normalized, out);
}

QueueResult queueReaderOpened(const std::string& hash, const std::string& modifiedDay) {
  const StateLock lock;
  if (!lock.acquired()) return QueueResult::IoError;
  return queueStatusLocked(hash, BookOrbitCatalogClient::ReadStatus::Reading, modifiedDay, true);
}

QueueResult queueFinished(const std::string& hash, const std::string& modifiedDay) {
  return queueStatus(hash, BookOrbitCatalogClient::ReadStatus::Read, modifiedDay);
}

QueueResult queueStatus(const std::string& hash, const BookOrbitCatalogClient::ReadStatus status,
                        const std::string& modifiedDay) {
  const StateLock lock;
  if (!lock.acquired()) return QueueResult::IoError;
  return queueStatusLocked(hash, status, modifiedDay, false);
}

QueueResult queueRating(const std::string& hash, const uint8_t stars, const std::string& modifiedDay) {
  if (stars > 5 || !validDay(modifiedDay)) return QueueResult::Invalid;
  std::string normalized;
  if (!normalizeHash(hash, normalized)) return QueueResult::Invalid;
  const StateLock lock;
  if (!lock.acquired()) return QueueResult::IoError;
  State state;
  if (!loadLocked(normalized, state)) return QueueResult::IoError;
  const bool cleared = stars == 0;
  if (state.pendingRating && state.rating == stars && state.ratingCleared == cleared &&
      state.ratingModified == modifiedDay) return QueueResult::Queued;
  state.pendingRating = true;
  state.rating = stars;
  state.ratingCleared = cleared;
  state.ratingModified = modifiedDay;
  state.ratingRevision = nextRevision(state);
  if (!writePendingLocked(state)) return QueueResult::IoError;
  if (!writeCacheLocked(state)) LOG_ERR("BOState", "Pending rating saved but cache write failed");
  return QueueResult::Queued;
}

bool nextPending(const std::string& cursor, const bool includeCursor, State& out) {
  std::string normalizedCursor;
  if (!cursor.empty() && !normalizeHash(cursor, normalizedCursor)) return false;
  const StateLock lock;
  if (!lock.acquired()) return false;

  HalFile directory = Storage.open(PENDING_DIR);
  if (!directory || !directory.isDirectory()) return false;
  directory.rewindDirectory();
  char bestHash[33]{};
  char name[64]{};
  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    const size_t nameLength = strlen(name);
    const bool isState = nameLength == 38 && strcmp(name + 32, ".state") == 0;
    const bool isBackup = nameLength == 42 && strcmp(name + 32, ".state.bak") == 0;
    // A backup-only filename must still reach loadLocked() for crash recovery.
    if (!isState && !isBackup) continue;
    char candidate[33]{};
    bool valid = true;
    for (size_t i = 0; i < 32; ++i) {
      char digit = name[i];
      if (digit >= 'A' && digit <= 'F') digit = static_cast<char>(digit - 'A' + 'a');
      if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
        valid = false;
        break;
      }
      candidate[i] = digit;
    }
    if (!valid || (normalizedCursor.size() == 32 &&
                   (strcmp(candidate, normalizedCursor.c_str()) < 0 ||
                    (!includeCursor && strcmp(candidate, normalizedCursor.c_str()) == 0)))) {
      continue;
    }
    if (!bestHash[0] || strcmp(candidate, bestHash) < 0) memcpy(bestHash, candidate, sizeof(bestHash));
  }
  if (!bestHash[0] || !loadLocked(bestHash, out)) return false;
  return out.pendingStatus || out.pendingRating;
}

bool rememberMapping(const std::string& hash, const int32_t bookId, const int32_t bookFileId,
                     const std::string& libraryVersion) {
  std::string normalized;
  if (!normalizeHash(hash, normalized) || bookId <= 0 || bookFileId <= 0 || libraryVersion.size() >= 32) return false;
  const StateLock lock;
  if (!lock.acquired()) return false;
  State state;
  if (!loadLocked(normalized, state)) return false;
  state.bookId = bookId;
  state.bookFileId = bookFileId;
  state.libraryVersion = libraryVersion;
  return writeCacheLocked(state);
}

bool rememberCanonicalStatus(const std::string& hash, const BookOrbitCatalogClient::ReadStatus status) {
  std::string normalized;
  if (!normalizeHash(hash, normalized)) return false;
  const StateLock lock;
  if (!lock.acquired()) return false;
  State state;
  if (!loadLocked(normalized, state)) return false;
  state.canonicalStatus = status;
  state.canonicalStatusSet = true;
  return writeCacheLocked(state);
}

bool rememberCanonicalRating(const std::string& hash, const bool isSet, const int8_t rating,
                             const std::string& updatedAt) {
  std::string normalized;
  if (!normalizeHash(hash, normalized) || (isSet && (rating < 1 || rating > 5)) || updatedAt.size() >= 32) return false;
  const StateLock lock;
  if (!lock.acquired()) return false;
  State state;
  if (!loadLocked(normalized, state)) return false;
  // Canonical server state is independent of a pending local edit. Compare
  // server timestamps when both are known; equal-day local conflict resolution
  // belongs to the pending-edit path, not this canonical cache.
  if (!updatedAt.empty() && !state.ratingUpdatedAt.empty() && !newerOrEqual(updatedAt, state.ratingUpdatedAt)) {
    return true;
  }
  state.canonicalRatingSet = isSet;
  state.canonicalRating = isSet ? rating : -1;
  state.ratingUpdatedAt = updatedAt;
  return writeCacheLocked(state);
}

bool acknowledge(const State& sent, const bool acknowledgeStatus, const bool acknowledgeRating) {
  std::string normalized;
  if (!normalizeHash(sent.hash, normalized)) return false;
  const StateLock lock;
  if (!lock.acquired()) return false;
  State current;
  if (!loadLocked(normalized, current)) return false;
  if (acknowledgeStatus && current.pendingStatus && current.statusRevision == sent.statusRevision) {
    current.pendingStatus = false;
    current.statusModified.clear();
  }
  if (acknowledgeRating && current.pendingRating && current.ratingRevision == sent.ratingRevision) {
    current.pendingRating = false;
    current.ratingCleared = false;
    current.rating = 0;
    current.ratingModified.clear();
  }
  // Publish the cleared cache first. If power fails before the pending file is removed,
  // startup overlays it and retries idempotently; it can never silently lose an event.
  if (!writeCacheLocked(current)) return false;
  return writePendingLocked(current);
}

bool beginReviewUpdate(const std::string& hash, const std::string& updatedAt, ReviewNoteWriter& writer) {
  std::string normalized;
  if (writer.active || !normalizeHash(hash, normalized) || updatedAt.size() >= 27) return false;
  if (xSemaphoreTake(reviewMutex(), portMAX_DELAY) != pdTRUE) return false;
  writer.hash = normalized;
  writer.updatedAt = updatedAt;
  writer.length = 0;
  writer.crcState = 0xFFFFFFFFu;
  snprintf(writer.tempPath.data(), writer.tempPath.size(), "%s/%s.tmp", REVIEW_DIR, normalized.c_str());
  if (!Storage.ensureDirectoryExists(ROOT) || !Storage.ensureDirectoryExists(REVIEW_DIR) ||
      !Storage.openFileForWrite("BOState", writer.tempPath.data(), writer.file)) {
    xSemaphoreGive(reviewMutex());
    return false;
  }
  std::array<uint8_t, NOTE_HEADER_BYTES> blank{};
  if (writer.file.write(blank.data(), blank.size()) != blank.size()) {
    writer.file = HalFile{};
    Storage.remove(writer.tempPath.data());
    xSemaphoreGive(reviewMutex());
    return false;
  }
  writer.active = true;
  return true;
}

bool writeReviewChunk(ReviewNoteWriter& writer, const uint8_t* bytes, const size_t length) {
  if (!writer.active || (length && !bytes) || length > MAX_REVIEW_BYTES - writer.length) return false;
  if (length && writer.file.write(bytes, length) != length) return false;
  writer.crcState = updateCrc(writer.crcState, bytes, length);
  writer.length += static_cast<uint32_t>(length);
  return true;
}

bool finishReviewUpdate(ReviewNoteWriter& writer, const bool noteSet, const std::string& updatedAt, State& out) {
  if (!writer.active || updatedAt.size() >= 27) {
    abortReviewUpdate(writer);
    return false;
  }
  writer.updatedAt = updatedAt;
  const StateLock lock;
  if (!lock.acquired()) {
    abortReviewUpdate(writer);
    return false;
  }
  State state;
  if (!loadLocked(writer.hash, state)) {
    abortReviewUpdate(writer);
    return false;
  }
  const std::string previousPath = state.reviewNoteSet ? notePath(state) : std::string{};
  // An unset canonical note may legitimately have no server timestamp.
  if ((noteSet || !writer.updatedAt.empty()) && !newerOrEqual(writer.updatedAt, state.reviewUpdatedAt)) {
    abortReviewUpdate(writer);
    out = state;
    return true;
  }

  if (!noteSet) {
    writer.file = HalFile{};
    Storage.remove(writer.tempPath.data());
    state.reviewNoteSet = false;
    state.reviewLength = 0;
    state.reviewFileGeneration = 0;
    state.reviewCrc = 0;
    state.reviewUpdatedAt = writer.updatedAt;
    if (!writeCacheLocked(state)) {
      abortReviewUpdate(writer);
      return false;
    }
    if (!previousPath.empty()) Storage.remove(previousPath.c_str());
    writer.active = false;
    xSemaphoreGive(reviewMutex());
    out = state;
    return true;
  }

  const uint32_t crc = writer.crcState ^ 0xFFFFFFFFu;
  std::array<uint8_t, NOTE_HEADER_BYTES> header{};
  encodeNoteHeader(header.data(), writer.hash, writer.updatedAt, writer.length, crc);
  if (!writer.file.seekSet64(0) || writer.file.write(header.data(), header.size()) != header.size()) {
    abortReviewUpdate(writer);
    return false;
  }
  writer.file.flush();
  writer.file = HalFile{};

  uint32_t generation = millis();
  std::array<char, 128> final{};
  for (;;) {
    makeNotePath(writer.hash, writer.updatedAt, crc, generation, final);
    if (!Storage.exists(final.data())) break;
    if (generation == UINT32_MAX) {
      abortReviewUpdate(writer);
      return false;
    }
    ++generation;
  }
  if (!Storage.rename(writer.tempPath.data(), final.data())) {
    abortReviewUpdate(writer);
    return false;
  }
  state.reviewNoteSet = true;
  state.reviewLength = writer.length;
  state.reviewFileGeneration = generation;
  state.reviewCrc = crc;
  state.reviewUpdatedAt = writer.updatedAt;
  if (!writeCacheLocked(state)) {
    Storage.remove(final.data());
    abortReviewUpdate(writer);
    return false;
  }
  if (!previousPath.empty() && previousPath != final.data()) Storage.remove(previousPath.c_str());
  writer.active = false;
  xSemaphoreGive(reviewMutex());
  out = state;
  return true;
}

void abortReviewUpdate(ReviewNoteWriter& writer) {
  if (!writer.active) return;
  writer.file = HalFile{};
  Storage.remove(writer.tempPath.data());
  writer.active = false;
  xSemaphoreGive(reviewMutex());
}

bool verifyReview(const std::string& hash) {
  State state;
  if (!read(hash, state) || !state.reviewNoteSet) return false;
  HalFile file;
  const std::string path = notePath(state);
  if (!Storage.openFileForRead("BOState", path, file) || !verifyNoteHeader(file, state) ||
      !file.seekSet64(NOTE_HEADER_BYTES)) return false;
  std::array<uint8_t, 256> buffer{};
  uint32_t crc = 0xFFFFFFFFu;
  size_t remaining = state.reviewLength;
  while (remaining > 0) {
    const size_t count = std::min(remaining, buffer.size());
    if (file.read(buffer.data(), count) != static_cast<int>(count)) return false;
    crc = updateCrc(crc, buffer.data(), count);
    remaining -= count;
  }
  return (crc ^ 0xFFFFFFFFu) == state.reviewCrc;
}

bool readReviewChunk(const std::string& hash, const uint32_t offset, uint8_t* out,
                     const size_t capacity, size_t& bytesRead) {
  bytesRead = 0;
  if (capacity && !out) return false;
  State state;
  if (!read(hash, state) || !state.reviewNoteSet || offset > state.reviewLength) return false;
  HalFile file;
  const std::string path = notePath(state);
  if (!Storage.openFileForRead("BOState", path, file) || !verifyNoteHeader(file, state) ||
      !file.seekSet64(NOTE_HEADER_BYTES + offset)) return false;
  const size_t count = std::min(capacity, static_cast<size_t>(state.reviewLength - offset));
  if (count == 0) return true;
  const int readCount = file.read(out, count);
  if (readCount < 0) return false;
  bytesRead = static_cast<size_t>(readCount);
  return bytesRead == count;
}

bool readReviewWindow(const std::string& hash, const uint32_t offset, const uint32_t noteLength, uint8_t* out,
                      const size_t capacity, size_t& bytesRead, uint32_t& nextOffset) {
  bytesRead = 0;
  nextOffset = offset;
  if (!out || capacity == 0 || offset > noteLength) return false;

  size_t rawBytes = 0;
  if (!readReviewChunk(hash, offset, out, capacity, rawBytes) || rawBytes > capacity ||
      rawBytes > noteLength - offset || (rawBytes == 0 && offset < noteLength)) {
    return false;
  }
  size_t safeBytes = rawBytes;
  if (offset + rawBytes < noteLength) {
    size_t leadAt = safeBytes;
    while (leadAt > 0 && (out[leadAt - 1] & 0xC0u) == 0x80u) --leadAt;
    if (leadAt > 0) {
      const uint8_t lead = out[leadAt - 1];
      const size_t expected = lead >= 0xF0u && lead <= 0xF4u ? 4
                              : lead >= 0xE0u && lead <= 0xEFu ? 3
                              : lead >= 0xC2u && lead <= 0xDFu ? 2
                                                               : 1;
      if (safeBytes - leadAt + 1 < expected) safeBytes = leadAt - 1;
    }
  }
  if (safeBytes == 0 && offset < noteLength) return false;
  bytesRead = safeBytes;
  nextOffset = offset + static_cast<uint32_t>(safeBytes);
  return true;
}

}  // namespace BookOrbitBookStateStore
