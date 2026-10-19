#include "BookOrbitReadingStatsOutbox.h"

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <cctype>
#include <cstring>

#include "ReadingStatsSlotFile.h"

namespace {
constexpr char PATH[] = "/.crosspoint/bookorbit-reading-stats.bin";
constexpr char TEMP_PATH[] = "/.crosspoint/bookorbit-reading-stats.tmp";
constexpr char BACKUP_PATH[] = "/.crosspoint/bookorbit-reading-stats.bak";
constexpr uint8_t FILE_MAGIC[] = {'B', 'O', 'S', 1};
constexpr size_t HASH_BYTES = 32;
constexpr size_t RECORD_BYTES = HASH_BYTES + 5 * sizeof(uint32_t);
constexpr size_t HEADER_BYTES = sizeof(FILE_MAGIC);
constexpr size_t UPLOAD_BATCH_EVENTS = 32;
bool mainJournalRecovered = false;

SemaphoreHandle_t journalMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}

class JournalLock {
 public:
  JournalLock() : mutex(journalMutex()), locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
  ~JournalLock() {
    if (locked) xSemaphoreGive(mutex);
  }
  bool acquired() const { return locked; }

 private:
  SemaphoreHandle_t mutex;
  bool locked;
};

void encodeU32(uint8_t* out, const uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t decodeU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

bool encodeRecord(const std::string& hash, const BookOrbitPageStatEvent& event, uint8_t* out) {
  ReadingStatsSlotFile::DocKey key{};
  if (!ReadingStatsSlotFile::parseDocId(hash, key) || event.startTime == 0 || event.durationSeconds > 86400 ||
      event.totalPages == 0) {
    return false;
  }
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < key.size(); ++i) {
    out[2 * i] = static_cast<uint8_t>(HEX_DIGITS[key[i] >> 4]);
    out[2 * i + 1] = static_cast<uint8_t>(HEX_DIGITS[key[i] & 0x0F]);
  }
  encodeU32(out + HASH_BYTES, event.page);
  encodeU32(out + HASH_BYTES + 4, event.startTime);
  encodeU32(out + HASH_BYTES + 8, event.durationSeconds);
  encodeU32(out + HASH_BYTES + 12, event.totalPages);
  encodeU32(out + HASH_BYTES + 16, ReadingStatsSlotFile::crc32(out, HASH_BYTES + 16));
  return true;
}

bool decodeRecord(const uint8_t* in, std::array<char, HASH_BYTES + 1>& hash, BookOrbitPageStatEvent& event) {
  if (decodeU32(in + HASH_BYTES + 16) != ReadingStatsSlotFile::crc32(in, HASH_BYTES + 16)) return false;
  for (size_t i = 0; i < HASH_BYTES; ++i) {
    const unsigned char digit = in[i];
    if (!std::isxdigit(digit)) return false;
    hash[i] = static_cast<char>(std::tolower(digit));
  }
  hash[HASH_BYTES] = char{};
  event.page = decodeU32(in + HASH_BYTES);
  event.startTime = decodeU32(in + HASH_BYTES + 4);
  event.durationSeconds = decodeU32(in + HASH_BYTES + 8);
  event.totalPages = decodeU32(in + HASH_BYTES + 12);
  return event.startTime > 0 && event.durationSeconds <= 86400 && event.totalPages > 0;
}

bool readHeader(HalFile& file) {
  std::array<uint8_t, HEADER_BYTES> magic{};
  return file.seekSet64(0) && file.read(magic.data(), magic.size()) == magic.size() &&
         memcmp(magic.data(), FILE_MAGIC, magic.size()) == 0;
}

bool writeHeader(HalFile& file) { return file.write(FILE_MAGIC, HEADER_BYTES) == HEADER_BYTES; }

bool readRecord(HalFile& file, uint8_t* record) { return file.read(record, RECORD_BYTES) == RECORD_BYTES; }

bool sameEvent(const BookOrbitPageStatEvent& left, const BookOrbitPageStatEvent& right) {
  return left.page == right.page && left.startTime == right.startTime && left.durationSeconds == right.durationSeconds &&
         left.totalPages == right.totalPages;
}

enum class JournalValidity { Missing, Valid, RecoverableTail, Invalid };

struct JournalScan {
  JournalValidity validity;
  size_t validRecords;
};

JournalScan scanJournal(const char* path) {
  if (!Storage.exists(path)) return {JournalValidity::Missing, 0};
  HalFile input;
  if (!Storage.openFileForRead("BOStats", path, input) || input.size64() < HEADER_BYTES || !readHeader(input)) {
    return {JournalValidity::Invalid, 0};
  }
  const uint64_t dataBytes = input.size64() - HEADER_BYTES;
  const uint64_t completeRecords = dataBytes / RECORD_BYTES;
  const bool partialTail = dataBytes % RECORD_BYTES != 0;
  size_t validRecords = 0;
  std::array<uint8_t, RECORD_BYTES> record{};
  std::array<char, HASH_BYTES + 1> hash{};
  BookOrbitPageStatEvent event{};
  for (uint64_t index = 0; index < completeRecords; ++index) {
    if (!readRecord(input, record.data()) || !decodeRecord(record.data(), hash, event)) {
      if (index + 1 != completeRecords) return {JournalValidity::Invalid, validRecords};
      return {JournalValidity::RecoverableTail, validRecords};
    }
    ++validRecords;
  }
  return {partialTail ? JournalValidity::RecoverableTail : JournalValidity::Valid, validRecords};
}

bool removeIfPresent(const char* path) {
  return !Storage.exists(path) || Storage.remove(path);
}

bool stageValidPrefix(const char* path, const size_t validRecords) {
  if (!removeIfPresent(TEMP_PATH)) return false;
  bool copied = false;
  {
    HalFile input;
    HalFile output;
    if (!Storage.openFileForRead("BOStats", path, input) || !readHeader(input) ||
        !Storage.openFileForWrite("BOStats", TEMP_PATH, output) || !writeHeader(output)) {
      return false;
    }
    std::array<uint8_t, RECORD_BYTES> record{};
    copied = true;
    for (size_t index = 0; index < validRecords; ++index) {
      if (!readRecord(input, record.data()) || output.write(record.data(), record.size()) != record.size()) {
        copied = false;
        break;
      }
    }
    output.flush();
  }
  return copied && scanJournal(TEMP_PATH).validity == JournalValidity::Valid;
}

bool cleanupRecoveryFiles() {
  return removeIfPresent(BACKUP_PATH) && removeIfPresent(TEMP_PATH);
}

// If publication stopped between the two FAT renames, prefer its fully staged image. Otherwise
// restore the backup; the caller then re-scans it so a recoverable append tail is preserved.
bool recoverMissingLivePath(bool& recovered) {
  const JournalScan temp = scanJournal(TEMP_PATH);
  if (temp.validity == JournalValidity::Valid) {
    if (!Storage.rename(TEMP_PATH, PATH) || scanJournal(PATH).validity != JournalValidity::Valid) return false;
    if (!removeIfPresent(BACKUP_PATH)) return false;
    recovered = true;
    return true;
  }

  const JournalScan backup = scanJournal(BACKUP_PATH);
  if (backup.validity == JournalValidity::Valid || backup.validity == JournalValidity::RecoverableTail) {
    if (!Storage.rename(BACKUP_PATH, PATH) || !removeIfPresent(TEMP_PATH)) return false;
    return false;
  }
  if (backup.validity == JournalValidity::Missing) {
    if (!removeIfPresent(TEMP_PATH)) return false;
    recovered = true;
    return true;
  }
  return false;
}

// Keep a known-good live prefix after an interrupted last append. A bad record in
// the middle is not guessed around: doing so could silently reorder or lose events.
bool recoverPathLocked(const char* path, const char* tempPath, bool& recovered) {
  if (recovered && !Storage.exists(BACKUP_PATH) && !Storage.exists(tempPath)) return true;
  recovered = false;
  for (;;) {
    const JournalScan live = scanJournal(path);
    if (live.validity == JournalValidity::Missing) {
      if (recoverMissingLivePath(recovered)) return true;
      if (!Storage.exists(path)) return false;
      continue;
    }
    if (live.validity == JournalValidity::Invalid) {
      LOG_ERR("BOStats", "Event journal is invalid; retaining it for recovery");
      return false;
    }
    if (live.validity == JournalValidity::Valid) {
      if (!cleanupRecoveryFiles()) return false;
      recovered = true;
      return true;
    }

    // A recoverable live prefix is newer than any leftover backup. Stage it before replacing
    // anything, then retire the stale backup only after the complete prefix is durable.
    if (!stageValidPrefix(path, live.validRecords)) {
      LOG_ERR("BOStats", "Could not stage recovered event journal");
      return false;
    }
    if (!removeIfPresent(BACKUP_PATH) ||
        !Storage.publishStagedFileWithBackup(tempPath, path, BACKUP_PATH)) {
      return false;
    }
    if (scanJournal(path).validity != JournalValidity::Valid || !removeIfPresent(BACKUP_PATH)) return false;
    LOG_INF("BOStats", "Discarded incomplete event-journal tail; kept %u complete events",
            static_cast<unsigned>(live.validRecords));
    recovered = true;
    return true;
  }
}

bool compactPrefixLocked(const std::string& normalizedHash, const std::vector<BookOrbitPageStatEvent>& expected) {
  size_t matched = 0;
  bool copied = false;
  {
    HalFile input;
    HalFile output;
    if (!Storage.openFileForRead("BOStats", PATH, input) || !readHeader(input) ||
        !Storage.openFileForWrite("BOStats", TEMP_PATH, output) || !writeHeader(output)) {
      Storage.remove(TEMP_PATH);
      return false;
    }
    std::array<uint8_t, RECORD_BYTES> record{};
    std::array<char, HASH_BYTES + 1> recordHash{};
    BookOrbitPageStatEvent event;
    copied = true;
    while (readRecord(input, record.data())) {
      if (!decodeRecord(record.data(), recordHash, event)) {
        copied = false;
        break;
      }
      if (normalizedHash.compare(0, HASH_BYTES, recordHash.data(), HASH_BYTES) == 0 &&
          matched < expected.size()) {
        if (!sameEvent(event, expected[matched])) {
          copied = false;
          break;
        }
        ++matched;
        continue;
      }
      if (output.write(record.data(), record.size()) != record.size()) {
        copied = false;
        break;
      }
    }
    output.flush();
  }
  if (!copied || matched != expected.size()) {
    Storage.remove(TEMP_PATH);
    return false;
  }
  if (scanJournal(TEMP_PATH).validity != JournalValidity::Valid ||
      !Storage.publishStagedFileWithBackup(TEMP_PATH, PATH, BACKUP_PATH)) {
    mainJournalRecovered = false;
    return false;
  }
  if (scanJournal(PATH).validity != JournalValidity::Valid || !removeIfPresent(BACKUP_PATH)) {
    mainJournalRecovered = false;
    return false;
  }
  mainJournalRecovered = true;
  return true;
}
}  // namespace

namespace BookOrbitReadingStatsOutbox {

bool append(const std::string& hash, const BookOrbitPageStatEvent& event) {
  const JournalLock lock;
  if (!lock.acquired() || !Storage.ensureDirectoryExists("/.crosspoint") ||
      !recoverPathLocked(PATH, TEMP_PATH, mainJournalRecovered)) return false;
  std::array<uint8_t, RECORD_BYTES> record{};
  if (!encodeRecord(hash, event, record.data())) return false;

  if (!Storage.exists(PATH)) {
    HalFile file;
    if (!Storage.openFileForWrite("BOStats", PATH, file) || !writeHeader(file) ||
        file.write(record.data(), record.size()) != record.size()) {
      mainJournalRecovered = false;
      return false;
    }
    file.flush();
    mainJournalRecovered = true;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForUpdate("BOStats", PATH, file) || !readHeader(file) ||
      !file.seekSet64(file.size64())) {
    LOG_ERR("BOStats", "Could not append event to journal");
    return false;
  }
  if (file.write(record.data(), record.size()) != record.size()) {
    mainJournalRecovered = false;
    return false;
  }
  file.flush();
  mainJournalRecovered = true;
  return true;
}

bool nextBatch(BookOrbitPageStatsBook& out, const std::string& cursor, const bool includeCursor) {
  out = {};
  const JournalLock lock;
  if (!lock.acquired() || !recoverPathLocked(PATH, TEMP_PATH, mainJournalRecovered)) return false;

  std::array<uint8_t, RECORD_BYTES> record{};
  std::array<char, HASH_BYTES + 1> hash{};
  BookOrbitPageStatEvent event;
  std::array<char, HASH_BYTES + 1> selectedHash{};
  bool hasSelectedHash = false;
  {
    HalFile file;
    if (!Storage.openFileForRead("BOStats", PATH, file) || !readHeader(file)) return false;
    while (file.position64() < file.size64()) {
      if (!readRecord(file, record.data()) || !decodeRecord(record.data(), hash, event)) return false;
      const int cursorComparison = std::strcmp(hash.data(), cursor.c_str());
      if (cursorComparison < 0 || (!includeCursor && cursorComparison == 0)) continue;
      if (!hasSelectedHash || std::strcmp(hash.data(), selectedHash.data()) < 0) {
        std::memcpy(selectedHash.data(), hash.data(), HASH_BYTES + 1);
        hasSelectedHash = true;
      }
    }
  }
  if (!hasSelectedHash) return false;

  HalFile file;
  if (!Storage.openFileForRead("BOStats", PATH, file) || !readHeader(file)) return false;
  out.hash.assign(selectedHash.data(), HASH_BYTES);
  out.events.reserve(UPLOAD_BATCH_EVENTS);
  while (file.position64() < file.size64() && out.events.size() < UPLOAD_BATCH_EVENTS) {
    if (!readRecord(file, record.data()) || !decodeRecord(record.data(), hash, event)) {
      out = {};
      return false;
    }
    if (std::strcmp(hash.data(), selectedHash.data()) == 0) out.events.push_back(event);
  }
  return !out.events.empty();
}

bool acknowledge(const std::string& hash, const std::vector<BookOrbitPageStatEvent>& events) {
  if (events.empty() || events.size() > UPLOAD_BATCH_EVENTS) return false;
  ReadingStatsSlotFile::DocKey key{};
  if (!ReadingStatsSlotFile::parseDocId(hash, key)) return false;
  const std::string normalized = ReadingStatsSlotFile::formatDocId(key);
  const JournalLock lock;
  if (!lock.acquired() || !recoverPathLocked(PATH, TEMP_PATH, mainJournalRecovered)) return false;
  return compactPrefixLocked(normalized, events);
}


}  // namespace BookOrbitReadingStatsOutbox
