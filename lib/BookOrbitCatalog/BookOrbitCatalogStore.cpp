#include "BookOrbitCatalogStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "BookOrbitCatalogSettings.h"

namespace {
constexpr char LIVE[] = "/.crosspoint/bookorbit/catalog.bin";
constexpr char TEMP[] = "/.crosspoint/bookorbit/catalog.tmp";
constexpr char BACKUP[] = "/.crosspoint/bookorbit/catalog.bak";
constexpr char BOOK_IDS[] = "/.crosspoint/bookorbit/catalog.ids.work";
constexpr char BOOK_IDS_GROW[] = "/.crosspoint/bookorbit/catalog.ids.grow";
constexpr char SOURCE_LIBRARY_IDS[] = "/.crosspoint/bookorbit/source-library.ids.work";
constexpr char SOURCE_LIBRARY_IDS_GROW[] = "/.crosspoint/bookorbit/source-library.ids.grow";
constexpr char SOURCE_COLLECTION_IDS[] = "/.crosspoint/bookorbit/source-collection.ids.work";
constexpr char SOURCE_COLLECTION_IDS_GROW[] = "/.crosspoint/bookorbit/source-collection.ids.grow";
constexpr char SOURCE_SMART_SCOPE_IDS[] = "/.crosspoint/bookorbit/source-smart-scope.ids.work";
constexpr char SOURCE_SMART_SCOPE_IDS_GROW[] = "/.crosspoint/bookorbit/source-smart-scope.ids.grow";
constexpr char BOOK_INDEX_TEMP[] = "/.crosspoint/bookorbit/catalog.ids.tmp";
constexpr char BOOK_INDEX_LIVE[] = "/.crosspoint/bookorbit/catalog.ids";
constexpr char BOOK_INDEX_BUILD[] = "/.crosspoint/bookorbit/catalog.ids.lookup";
constexpr char BOOK_INDEX_BUILD_GROW[] = "/.crosspoint/bookorbit/catalog.ids.lookup.grow";
constexpr char BOOK_INDEX_COUNTS_WORK[] = "/.crosspoint/bookorbit/catalog.ids.counts.work";
constexpr char BOOK_LABEL_SET_WORK[] = "/.crosspoint/bookorbit/catalog.labels.work";
constexpr char BOOK_LABEL_SET_GROW[] = "/.crosspoint/bookorbit/catalog.labels.grow";
constexpr char BOOK_LABEL_DATA_WORK[] = "/.crosspoint/bookorbit/catalog.labels.data.work";
constexpr uint32_t FILE_MAGIC = 0x424F4341;
constexpr uint32_t DESC_MAGIC = 0x44455343;
constexpr uint32_t ROW_MAGIC = 0x524F5721;
constexpr uint32_t BOOK_INDEX_MAGIC = 0x424F4944;
constexpr uint16_t VERSION = 4;
constexpr uint16_t BOOK_INDEX_VERSION = 2;
constexpr uint16_t MAX_URL_BYTES = 512;
constexpr uint16_t MAX_FIELD_BYTES = 512;
constexpr uint32_t NO_LINK = UINT32_MAX;
constexpr uint32_t BOOK_PAGE_SIZE = 100;
constexpr uint32_t LOCAL_PAGE_SIZE = 100;
constexpr char SEARCH_PREFIX[] = "@offline-search:";
constexpr char LOCAL_PREFIX[] = "@offline:";
constexpr uint8_t LIST_NORMAL = 0;
constexpr uint8_t LIST_ALL_BOOKS = 1;
constexpr uint8_t LIST_SCOPED_BOOKS = 2;
enum : uint8_t { RECORD_DESCRIPTOR = 1, RECORD_ROW = 2 };
enum class RowRole : uint8_t { Listing, CanonicalBook, ShelfReference };

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;
  uint32_t descriptorCount;
  uint32_t rowCount;
  uint16_t baseLength;
  uint16_t usernameLength;
  int32_t canonicalTotal;
};
struct DescriptorRecord {
  uint32_t magic;
  uint8_t kind;
  uint8_t complete;
  uint8_t canonicalBooks;
  uint8_t reserved;
  uint16_t urlLength;
  int32_t total;
  int32_t page;
  uint32_t nextIndex;
  uint32_t prevIndex;
};
struct RowRecord {
  uint32_t magic;
  uint8_t kind;
  uint8_t type;
  uint8_t group;
  uint8_t hasCover;
  RowRole role;
  uint8_t reserved[3];
  uint32_t ownerIndex;
  int32_t bookId;
  int32_t count;
  int16_t progress;
  uint16_t fieldLength[5];
};
struct BookIdSlot {
  int32_t id;
  uint32_t reserved;
  uint64_t rowOffset;
};
struct BookIndexHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;
  uint64_t sourceFileSize;
  uint64_t sourceFingerprint;
  uint32_t sourceRowCount;
  int32_t canonicalTotal;
  uint32_t capacity;
  uint16_t baseLength;
  uint16_t usernameLength;
  uint32_t accountHash;
  uint32_t familyCountOffset;
  uint32_t familyCountCount;
};
struct FamilyCountHeader {
  uint16_t urlLength;
  uint16_t reserved;
  uint32_t count;
};
struct Descriptor {
  uint64_t offset = 0;
  DescriptorRecord record{};
  std::string url;
};
static_assert(sizeof(Header) == 24 && sizeof(DescriptorRecord) <= 32 && sizeof(RowRecord) <= 48 &&
              sizeof(BookIdSlot) == 16 && sizeof(BookIndexHeader) <= 56 && sizeof(FamilyCountHeader) == 8);

uint32_t accountHash();

struct SnapshotFingerprintCache {
  bool valid = false;
  uint64_t fileSize = 0;
  uint32_t descriptorCount = 0;
  uint32_t rowCount = 0;
  int32_t canonicalTotal = 0;
  uint16_t baseLength = 0;
  uint16_t usernameLength = 0;
  uint32_t account = 0;
  uint64_t fingerprint = 0;
};

SnapshotFingerprintCache snapshotFingerprintCache;

void invalidateSnapshotFingerprint() { snapshotFingerprintCache.valid = false; }

void cacheSnapshotFingerprint(const Header& header, uint64_t fileSize, uint64_t fingerprint) {
  snapshotFingerprintCache.valid = true;
  snapshotFingerprintCache.fileSize = fileSize;
  snapshotFingerprintCache.descriptorCount = header.descriptorCount;
  snapshotFingerprintCache.rowCount = header.rowCount;
  snapshotFingerprintCache.canonicalTotal = header.canonicalTotal;
  snapshotFingerprintCache.baseLength = header.baseLength;
  snapshotFingerprintCache.usernameLength = header.usernameLength;
  snapshotFingerprintCache.account = accountHash();
  snapshotFingerprintCache.fingerprint = fingerprint;
}

bool computeSnapshotFingerprint(HalFile& file, uint64_t& fingerprint) {
  constexpr uint64_t FNV_BASIS = 14695981039346656037ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;
  uint8_t buffer[128];
  uint64_t hash = FNV_BASIS;
  uint64_t remaining = file.fileSize64();
  if (!file.seekSet64(0)) return false;
  while (remaining) {
    const size_t bytes = remaining < sizeof(buffer) ? static_cast<size_t>(remaining) : sizeof(buffer);
    if (file.read(buffer, bytes) != static_cast<int>(bytes)) return false;
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ buffer[i]) * FNV_PRIME;
    remaining -= bytes;
  }
  fingerprint = hash;
  return true;
}

bool fingerprintForSnapshot(HalFile& file, const Header& header, uint64_t& fingerprint) {
  const uint64_t fileSize = file.fileSize64();
  const uint32_t account = accountHash();
  if (snapshotFingerprintCache.valid && snapshotFingerprintCache.fileSize == fileSize &&
      snapshotFingerprintCache.descriptorCount == header.descriptorCount &&
      snapshotFingerprintCache.rowCount == header.rowCount &&
      snapshotFingerprintCache.canonicalTotal == header.canonicalTotal &&
      snapshotFingerprintCache.baseLength == header.baseLength &&
      snapshotFingerprintCache.usernameLength == header.usernameLength && snapshotFingerprintCache.account == account) {
    fingerprint = snapshotFingerprintCache.fingerprint;
    return true;
  }
  if (!computeSnapshotFingerprint(file, fingerprint)) return false;
  cacheSnapshotFingerprint(header, fileSize, fingerprint);
  return true;
}
bool writeExact(HalFile& file, const void* data, size_t size) { return file.write(data, size) == size; }
bool readExact(HalFile& file, void* data, size_t size) { return file.read(data, size) == static_cast<int>(size); }

uint32_t hashBookId(int32_t id) {
  uint32_t value = static_cast<uint32_t>(id);
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  return value;
}
class DiskBookIdSet {
 public:
  explicit DiskBookIdSet(const char* tablePath = BOOK_IDS, const char* growPath = BOOK_IDS_GROW)
      : tablePath(tablePath), growPath(growPath) {}
  DiskBookIdSet(const DiskBookIdSet&) = delete;
  DiskBookIdSet& operator=(const DiskBookIdSet&) = delete;
  ~DiskBookIdSet() { close(); }

  void close() {
    if (capacity == 0) return;
    if (file) file.close();
    Storage.remove(tablePath);
    Storage.remove(growPath);
    capacity = 0;
    count = 0;
  }

  bool open() {
    close();
    capacity = 256;
    HalFile created;
    if (!Storage.openFileForWrite("BOCat", tablePath, created) || !writeEmptySlots(created, capacity) ||
        !created.close())
      return false;
    return Storage.openFileForUpdate("BOCat", tablePath, file);
  }

  bool insert(int32_t id, bool& alreadyPresent, uint64_t& rowOffset) {
    if (id <= 0 || capacity == 0) return false;
    if (count >= capacity / 2 && !grow()) return false;
    return insertInto(file, capacity, id, alreadyPresent, rowOffset, &count);
  }

  bool setOffset(int32_t id, uint64_t rowOffset) {
    if (capacity == 0) return false;
    uint32_t slot = hashBookId(id) & (capacity - 1);
    for (uint32_t examined = 0; examined < capacity; ++examined, slot = (slot + 1) & (capacity - 1)) {
      BookIdSlot value{};
      if (!readSlot(file, slot, value)) return false;
      if (value.id == 0) return false;
      if (value.id != id) continue;
      value.rowOffset = rowOffset;
      return writeSlot(file, slot, value);
    }
    return false;
  }

  bool setReserved(int32_t id, uint32_t reserved) {
    if (id <= 0 || capacity == 0) return false;
    uint32_t slot = hashBookId(id) & (capacity - 1);
    for (uint32_t examined = 0; examined < capacity; ++examined, slot = (slot + 1) & (capacity - 1)) {
      BookIdSlot value{};
      if (!readSlot(file, slot, value)) return false;
      if (value.id == 0) return false;
      if (value.id != id) continue;
      value.reserved = reserved;
      return writeSlot(file, slot, value);
    }
    return false;
  }

  bool lookupValues(int32_t id, uint32_t& reserved, uint64_t& rowOffset, bool& found) {
    found = false;
    if (id <= 0 || capacity == 0) return false;
    uint32_t slot = hashBookId(id) & (capacity - 1);
    for (uint32_t examined = 0; examined < capacity; ++examined, slot = (slot + 1) & (capacity - 1)) {
      BookIdSlot value{};
      if (!readSlot(file, slot, value)) return false;
      if (value.id == 0) return true;
      if (value.id != id) continue;
      reserved = value.reserved;
      rowOffset = value.rowOffset;
      found = true;
      return true;
    }
    return true;
  }

  bool lookup(int32_t id, uint64_t& rowOffset) {
    if (id <= 0 || capacity == 0) return false;
    uint32_t slot = hashBookId(id) & (capacity - 1);
    for (uint32_t examined = 0; examined < capacity; ++examined, slot = (slot + 1) & (capacity - 1)) {
      BookIdSlot value{};
      if (!readSlot(file, slot, value)) return false;
      if (value.id == 0) return false;
      if (value.id == id) {
        rowOffset = value.rowOffset;
        return rowOffset != 0;
      }
    }
    return false;
  }

  bool copyTo(HalFile& target, uint64_t offset) {
    BookIdSlot slots[8]{};
    for (uint32_t slot = 0; slot < capacity;) {
      const uint32_t chunk = std::min<uint32_t>(8, capacity - slot);
      const size_t bytes = static_cast<size_t>(chunk) * sizeof(BookIdSlot);
      if (!file.seekSet64(static_cast<uint64_t>(slot) * sizeof(BookIdSlot)) || !readExact(file, slots, bytes) ||
          !target.seekSet64(offset + static_cast<uint64_t>(slot) * sizeof(BookIdSlot)) ||
          !writeExact(target, slots, bytes))
        return false;
      slot += chunk;
    }
    return true;
  }

  uint32_t size() const { return count; }
  uint32_t slotCount() const { return capacity; }

 private:
  static bool writeEmptySlots(HalFile& target, uint32_t slots) {
    BookIdSlot zeros[8]{};
    uint32_t remaining = slots;
    if (!target.seekSet64(0)) return false;
    while (remaining) {
      const uint32_t chunk = std::min<uint32_t>(remaining, sizeof(zeros) / sizeof(zeros[0]));
      if (!writeExact(target, zeros, static_cast<size_t>(chunk) * sizeof(zeros[0]))) return false;
      remaining -= chunk;
    }
    return true;
  }

  static bool readSlot(HalFile& target, uint32_t slot, BookIdSlot& value) {
    return target.seekSet64(static_cast<uint64_t>(slot) * sizeof(value)) && readExact(target, &value, sizeof(value));
  }

  static bool writeSlot(HalFile& target, uint32_t slot, const BookIdSlot& value) {
    return target.seekSet64(static_cast<uint64_t>(slot) * sizeof(value)) && writeExact(target, &value, sizeof(value));
  }

  static bool insertInto(HalFile& target, uint32_t slots, int32_t id, bool& alreadyPresent, uint64_t& rowOffset,
                         uint32_t* insertedCount) {
    const uint32_t mask = slots - 1;
    uint32_t slot = hashBookId(id) & mask;
    for (uint32_t examined = 0; examined < slots; ++examined, slot = (slot + 1) & mask) {
      BookIdSlot value{};
      if (!readSlot(target, slot, value)) return false;
      if (value.id == id) {
        alreadyPresent = true;
        rowOffset = value.rowOffset;
        return true;
      }
      if (value.id == 0) {
        value.id = id;
        if (!writeSlot(target, slot, value)) return false;
        alreadyPresent = false;
        rowOffset = 0;
        if (insertedCount) ++*insertedCount;
        return true;
      }
    }
    return false;
  }

  bool grow() {
    if (capacity > UINT32_MAX / 2) return false;
    const uint32_t nextCapacity = capacity * 2;
    file.close();
    Storage.remove(growPath);
    HalFile created;
    if (!Storage.openFileForWrite("BOCat", growPath, created) || !writeEmptySlots(created, nextCapacity) ||
        !created.close())
      return false;
    HalFile grown;
    HalFile prior;
    if (!Storage.openFileForUpdate("BOCat", growPath, grown) || !Storage.openFileForRead("BOCat", tablePath, prior))
      return false;
    for (uint32_t slot = 0; slot < capacity; ++slot) {
      BookIdSlot value{};
      bool duplicate = false;
      uint64_t ignored = 0;
      if (!readSlot(prior, slot, value) ||
          (value.id && !insertInto(grown, nextCapacity, value.id, duplicate, ignored, nullptr)) ||
          (value.id && !setSlot(grown, nextCapacity, value))) {
        prior.close();
        grown.close();
        return false;
      }
    }
    prior.close();
    if (!grown.close() || !Storage.remove(tablePath) || !Storage.rename(growPath, tablePath)) return false;
    capacity = nextCapacity;
    return Storage.openFileForUpdate("BOCat", tablePath, file);
  }

  static bool setSlot(HalFile& target, uint32_t slots, const BookIdSlot& value) {
    uint32_t slot = hashBookId(value.id) & (slots - 1);
    for (uint32_t examined = 0; examined < slots; ++examined, slot = (slot + 1) & (slots - 1)) {
      BookIdSlot existing{};
      if (!readSlot(target, slot, existing)) return false;
      if (existing.id == value.id) return writeSlot(target, slot, value);
      if (existing.id == 0) return false;
    }
    return false;
  }

  const char* tablePath;
  const char* growPath;
  HalFile file;
  uint32_t capacity = 0;
  uint32_t count = 0;
};
uint32_t accountHash() {
  constexpr uint32_t FNV_BASIS = 2166136261U;
  constexpr uint32_t FNV_PRIME = 16777619U;
  uint32_t hash = FNV_BASIS;
  const std::string base = KOREADER_STORE.getBaseUrl();
  const std::string& username = KOREADER_STORE.getUsername();
  for (const unsigned char value : base) hash = (hash ^ value) * FNV_PRIME;
  hash = (hash ^ 0xFFU) * FNV_PRIME;
  for (const unsigned char value : username) hash = (hash ^ value) * FNV_PRIME;
  return hash;
}

BookIndexHeader makeBookIndexHeader(HalFile& snapshot, const Header& header, uint32_t capacity, uint64_t fingerprint) {
  BookIndexHeader index{};
  index.magic = BOOK_INDEX_MAGIC;
  index.version = BOOK_INDEX_VERSION;
  index.headerBytes = sizeof(index);
  index.sourceFileSize = snapshot.fileSize64();
  index.sourceFingerprint = fingerprint;
  index.sourceRowCount = header.rowCount;
  index.canonicalTotal = header.canonicalTotal;
  index.capacity = capacity;
  index.baseLength = header.baseLength;
  index.usernameLength = header.usernameLength;
  index.accountHash = accountHash();
  return index;
}

bool writeBookIndexTemp(DiskBookIdSet& ids, HalFile& snapshot, const Header& header, uint64_t& fingerprint);

bool publishBookIndex() {
  Storage.remove(BOOK_INDEX_LIVE);
  if (!Storage.rename(BOOK_INDEX_TEMP, BOOK_INDEX_LIVE)) {
    Storage.remove(BOOK_INDEX_TEMP);
    return false;
  }
  return true;
}

uint64_t dataStart(const Header& header) { return sizeof(Header) + header.baseLength + header.usernameLength; }

bool headerValid(const Header& header) {
  return header.magic == FILE_MAGIC && header.version == VERSION && header.headerBytes == sizeof(Header);
}

bool readHeader(HalFile& file, Header& header, bool requireAccount) {
  if (!file.seekSet64(0) || !readExact(file, &header, sizeof(header)) || !headerValid(header) ||
      dataStart(header) > file.fileSize64())
    return false;
  const uint64_t recordBytes = file.fileSize64() - dataStart(header);
  if (header.descriptorCount > recordBytes / sizeof(DescriptorRecord) ||
      header.rowCount > recordBytes / sizeof(RowRecord))
    return false;
  if (!requireAccount) return true;
  const std::string base = KOREADER_STORE.getBaseUrl();
  const std::string& username = KOREADER_STORE.getUsername();
  if (base.size() != header.baseLength || username.size() != header.usernameLength) return false;
  char chunk[128];
  size_t matched = 0;
  while (matched < header.baseLength) {
    const size_t count = std::min(sizeof(chunk), static_cast<size_t>(header.baseLength) - matched);
    if (!file.seekSet64(sizeof(Header) + matched) || !readExact(file, chunk, count) ||
        memcmp(chunk, base.data() + matched, count) != 0)
      return false;
    matched += count;
  }
  matched = 0;
  while (matched < header.usernameLength) {
    const size_t count = std::min(sizeof(chunk), static_cast<size_t>(header.usernameLength) - matched);
    if (!file.seekSet64(sizeof(Header) + header.baseLength + matched) || !readExact(file, chunk, count) ||
        memcmp(chunk, username.data() + matched, count) != 0)
      return false;
    matched += count;
  }
  return true;
}

bool writeHeader(HalFile& file, const Header& header) {
  const uint64_t end = file.fileSize64();
  return file.seekSet64(0) && writeExact(file, &header, sizeof(header)) && file.seekSet64(end);
}

bool isBooksListUrl(const std::string& input) {
  if (input.empty()) return false;
  const size_t scheme = input.find("://");
  const size_t pathStart =
      scheme == std::string::npos
          ? 0
          : (input.find('/', scheme + 3) == std::string::npos ? input.size() : input.find('/', scheme + 3));
  const size_t query = input.find('?', pathStart);
  const size_t pathEnd = query == std::string::npos ? input.size() : query;
  const size_t pathLength = pathEnd - pathStart;
  return (pathLength == 5 && input.compare(pathStart, pathLength, "books") == 0) ||
         (pathLength == 6 && input.compare(pathStart, pathLength, "/books") == 0) ||
         (pathLength >= 6 && input.compare(pathEnd - 6, 6, "/books") == 0);
}

std::string withBookPageSize100(const std::string& input) {
  if (!isBooksListUrl(input)) return input;
  const size_t scheme = input.find("://");
  const size_t pathStart =
      scheme == std::string::npos
          ? 0
          : (input.find('/', scheme + 3) == std::string::npos ? input.size() : input.find('/', scheme + 3));
  const size_t query = input.find('?', pathStart);
  const size_t pathEnd = query == std::string::npos ? input.size() : query;
  std::string result = input.substr(0, pathEnd);
  if (query != std::string::npos) {
    size_t start = query + 1;
    while (start < input.size()) {
      const size_t end = input.find('&', start);
      const size_t partEnd = end == std::string::npos ? input.size() : end;
      if (partEnd - start < 5 || input.compare(start, 5, "size=") != 0) {
        result.push_back(result.find('?') == std::string::npos ? '?' : '&');
        result.append(input, start, partEnd - start);
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  result.push_back(result.find('?') == std::string::npos ? '?' : '&');
  result += "size=100";
  return result;
}

std::string normalizeUrl(const std::string& input) {
  if (input.empty()) return {};
  // Keep persisted descriptor identity intact; appendDescriptor rewrites new requests to size=100.
  std::string normalized = input;
  const size_t scheme = normalized.find("://");
  if (scheme != std::string::npos) {
    const size_t slash = normalized.find('/', scheme + 3);
    normalized = slash == std::string::npos ? std::string("/") : normalized.substr(slash);
  } else if (normalized[0] != '/') {
    normalized.insert(0, "/api/v1/koreader/plugin/catalog/");
  }
  size_t position = normalized.find("page=1");
  while (position != std::string::npos) {
    const size_t end = position + 6;
    const bool left = position > 0 && (normalized[position - 1] == '?' || normalized[position - 1] == '&');
    const bool right = end == normalized.size() || normalized[end] == '&';
    if (!left || !right) {
      position = normalized.find("page=1", end);
      continue;
    }
    size_t eraseStart = position;
    size_t eraseEnd = end;
    if (normalized[position - 1] == '?') {
      if (end < normalized.size())
        eraseEnd = end + 1;
      else
        eraseStart = position - 1;
    } else {
      eraseStart = position - 1;
    }
    normalized.erase(eraseStart, eraseEnd - eraseStart);
    position = normalized.find("page=1");
  }
  // Canonicalize query order for both descriptor identity and page families.
  // Swap adjacent tokens in place: no token copies or additional heap buffer.
  const size_t query = normalized.find('?');
  if (query != std::string::npos) {
    bool swapped;
    do {
      swapped = false;
      size_t start = query + 1;
      while (start < normalized.size()) {
        const size_t separator = normalized.find('&', start);
        if (separator == std::string::npos) break;
        const size_t next = separator + 1;
        const size_t end = normalized.find('&', next);
        const size_t tokenEnd = end == std::string::npos ? normalized.size() : end;
        const size_t nextLength = tokenEnd - next;
        if (normalized.compare(start, separator - start, normalized, next, nextLength) > 0) {
          std::reverse(normalized.begin() + start, normalized.begin() + separator);
          std::reverse(normalized.begin() + next, normalized.begin() + tokenEnd);
          std::reverse(normalized.begin() + start, normalized.begin() + tokenEnd);
          start += nextLength + 1;
          swapped = true;
        } else {
          start = next;
        }
      }
    } while (swapped);
  }
  return normalized;
}
std::string catalogPath(const std::string& url) {
  std::string normalized = normalizeUrl(url);
  const size_t query = normalized.find('?');
  if (query != std::string::npos) normalized.resize(query);
  return normalized;
}

bool isAggregateSection(const std::string& url, bool& authors) {
  const std::string path = catalogPath(url);
  if (path == "/api/v1/koreader/plugin/catalog/sections/authors") {
    authors = true;
    return true;
  }
  if (path == "/api/v1/koreader/plugin/catalog/sections/series") {
    authors = false;
    return true;
  }
  return false;
}

bool sourceKindForSection(const std::string& url, BookOrbitCatalogSettings::SourceKind& kind) {
  const std::string path = catalogPath(url);
  if (path == "/api/v1/koreader/plugin/catalog/sections/libraries")
    kind = BookOrbitCatalogSettings::SourceKind::Library;
  else if (path == "/api/v1/koreader/plugin/catalog/sections/collections")
    kind = BookOrbitCatalogSettings::SourceKind::Collection;
  else if (path == "/api/v1/koreader/plugin/catalog/sections/smart-scopes")
    kind = BookOrbitCatalogSettings::SourceKind::SmartScope;
  else
    return false;
  return true;
}

bool queryInt(const std::string& url, const char* key, int32_t& value) {
  const size_t query = url.find('?');
  if (query == std::string::npos) return false;
  const size_t keyLength = strlen(key);
  size_t start = query + 1;
  while (start < url.size()) {
    const size_t end = url.find('&', start);
    const size_t partEnd = end == std::string::npos ? url.size() : end;
    if (partEnd - start > keyLength && url.compare(start, keyLength, key) == 0 && url[start + keyLength] == '=') {
      const char* first = url.c_str() + start + keyLength + 1;
      char* parsedEnd = nullptr;
      const long parsed = strtol(first, &parsedEnd, 10);
      if (parsedEnd == first || parsed <= 0 || parsed > INT32_MAX || parsedEnd != url.c_str() + partEnd) return false;
      value = static_cast<int32_t>(parsed);
      return true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

bool sourceForHref(const std::string& href, BookOrbitCatalogSettings::SourceKind& kind, int32_t& id) {
  const BookOrbitCatalogSettings::SourceKind kinds[] = {
      BookOrbitCatalogSettings::SourceKind::Library,
      BookOrbitCatalogSettings::SourceKind::Collection,
      BookOrbitCatalogSettings::SourceKind::SmartScope,
  };
  for (const auto candidate : kinds) {
    if (queryInt(href, BookOrbitCatalogSettings::sourceQueryKey(candidate), id)) {
      kind = candidate;
      return true;
    }
  }
  return false;
}

bool selectedSource(const BookOrbitCatalogSettings& settings, BookOrbitCatalogSettings::SourceKind kind, int32_t id) {
  for (const auto& source : settings.sources)
    if (source.kind == kind && source.id == id) return true;
  return false;
}

std::string sourceBooksUrl(BookOrbitCatalogSettings::SourceKind kind, int32_t id) {
  return std::string("books?") + BookOrbitCatalogSettings::sourceQueryKey(kind) + "=" + std::to_string(id) +
         "&sort=title&size=100";
}

std::string pageFamilyUrl(const std::string& url) {
  const std::string normalized = normalizeUrl(url);
  const size_t query = normalized.find('?');
  if (query == std::string::npos) return normalized;
  std::string result = normalized.substr(0, query);
  result.reserve(normalized.size());
  size_t start = query + 1;
  while (start < normalized.size()) {
    const size_t end = normalized.find('&', start);
    const size_t partEnd = end == std::string::npos ? normalized.size() : end;
    if (normalized.compare(start, 5, "page=") != 0) {
      result.push_back(result.find('?') == std::string::npos ? '?' : '&');
      result.append(normalized, start, partEnd - start);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return result;
}

bool localRoute(const std::string& url, const char* route, uint32_t& page, std::string* label = nullptr) {
  const std::string prefix = std::string(LOCAL_PREFIX) + route + ":";
  if (url.compare(0, prefix.size(), prefix) != 0) return false;
  const size_t end = url.find(':', prefix.size());
  const size_t pageEnd = end == std::string::npos ? url.size() : end;
  if (pageEnd == prefix.size()) return false;
  char* parsedEnd = nullptr;
  const unsigned long parsed = strtoul(url.c_str() + prefix.size(), &parsedEnd, 10);
  if (parsedEnd != url.c_str() + pageEnd || parsed == 0 || parsed > INT32_MAX) return false;
  page = static_cast<uint32_t>(parsed);
  if (label) *label = end == std::string::npos ? std::string() : url.substr(end + 1);
  return true;
}

std::string localPageUrl(const char* route, uint32_t page, const std::string* label = nullptr) {
  std::string url = std::string(LOCAL_PREFIX) + route + ":" + std::to_string(page);
  if (label) {
    url.push_back(':');
    url += *label;
  }
  return url;
}

bool canonicalPageNumber(const std::string& url, uint32_t& page);

bool isCatalogHref(const std::string& href) {
  if (href.empty()) return true;
  const std::string base = KOREADER_STORE.getBaseUrl();
  const size_t baseScheme = base.find("://");
  const size_t hrefScheme = href.find("://");
  std::string path = href;
  if (hrefScheme != std::string::npos) {
    const size_t baseEnd = baseScheme == std::string::npos ? std::string::npos : base.find('/', baseScheme + 3);
    const size_t hrefEnd = href.find('/', hrefScheme + 3);
    if (base.substr(0, baseEnd) != href.substr(0, hrefEnd)) return false;
    path = hrefEnd == std::string::npos ? "/" : href.substr(hrefEnd);
  }
  if (path.find("..") != std::string::npos || path.find('#') != std::string::npos) return false;
  static constexpr char PREFIX[] = "/api/v1/koreader/plugin/catalog/";
  if (path[0] == '/') return path.compare(0, sizeof(PREFIX) - 1, PREFIX) == 0;
  return path == "root" || path == "dashboard" || path == "books" || path.compare(0, 6, "books?") == 0 ||
         path.compare(0, 6, "books/") == 0 || path.compare(0, 9, "sections/") == 0;
}

bool readRecordHeader(HalFile& file, uint64_t offset, uint64_t fileSize, uint8_t& kind, DescriptorRecord& descriptor,
                      RowRecord& row, uint64_t& nextOffset) {
  if (offset > fileSize || fileSize - offset < 5 || !file.seekSet64(offset)) return false;
  uint32_t magic = 0;
  if (!readExact(file, &magic, sizeof(magic)) || !readExact(file, &kind, sizeof(kind))) return false;
  if (kind == RECORD_DESCRIPTOR) {
    if (magic != DESC_MAGIC || fileSize - offset < sizeof(DescriptorRecord) || !file.seekSet64(offset) ||
        !readExact(file, &descriptor, sizeof(descriptor)) || descriptor.kind != RECORD_DESCRIPTOR ||
        descriptor.urlLength > MAX_URL_BYTES)
      return false;
    nextOffset = offset + sizeof(DescriptorRecord) + descriptor.urlLength;
  } else if (kind == RECORD_ROW) {
    if (magic != ROW_MAGIC || fileSize - offset < sizeof(RowRecord) || !file.seekSet64(offset) ||
        !readExact(file, &row, sizeof(row)) || row.kind != RECORD_ROW || row.type > 1 || row.group > 2 ||
        row.hasCover > 1 || row.role > RowRole::ShelfReference)
      return false;
    uint64_t length = sizeof(RowRecord);
    for (const uint16_t fieldLength : row.fieldLength) {
      if (fieldLength >= MAX_FIELD_BYTES) return false;
      length += fieldLength;
    }
    if (length > fileSize - offset) return false;
    nextOffset = offset + length;
  } else
    return false;
  return nextOffset > offset && nextOffset <= fileSize;
}
bool bookIndexHeaderMatches(const BookIndexHeader& index, HalFile& indexFile, HalFile& snapshot, const Header& header) {
  const uint64_t familyOffset = sizeof(BookIndexHeader) + static_cast<uint64_t>(index.capacity) * sizeof(BookIdSlot);
  if (index.magic != BOOK_INDEX_MAGIC || index.version != BOOK_INDEX_VERSION ||
      index.headerBytes != sizeof(BookIndexHeader) || index.sourceFileSize != snapshot.fileSize64() ||
      index.sourceRowCount != header.rowCount || index.canonicalTotal != header.canonicalTotal ||
      index.baseLength != header.baseLength || index.usernameLength != header.usernameLength ||
      index.accountHash != accountHash() || index.capacity < 256 || (index.capacity & (index.capacity - 1)) != 0 ||
      familyOffset > UINT32_MAX || index.familyCountOffset != familyOffset ||
      index.familyCountOffset > indexFile.fileSize64())
    return false;
  uint64_t fingerprint = 0;
  return fingerprintForSnapshot(snapshot, header, fingerprint) && index.sourceFingerprint == fingerprint;
}
bool readBookIndexHeader(HalFile& snapshot, const Header& header, BookIndexHeader& result, HalFile& indexFile) {
  return Storage.openFileForRead("BOCat", BOOK_INDEX_LIVE, indexFile) &&
         readExact(indexFile, &result, sizeof(result)) && bookIndexHeaderMatches(result, indexFile, snapshot, header);
}

bool lookupFamilyCount(HalFile& snapshot, const Header& header, const std::string& family, uint32_t& count,
                       bool& found) {
  found = false;
  count = 0;
  HalFile indexFile;
  BookIndexHeader indexHeader{};
  if (!readBookIndexHeader(snapshot, header, indexHeader, indexFile)) return false;
  const uint64_t indexFileSize = indexFile.fileSize64();
  if (indexHeader.familyCountOffset > indexFileSize ||
      static_cast<uint64_t>(indexHeader.familyCountCount) * sizeof(FamilyCountHeader) >
          indexFileSize - indexHeader.familyCountOffset)
    return false;
  uint64_t offset = indexHeader.familyCountOffset;
  std::string candidate;
  candidate.reserve(MAX_URL_BYTES);
  for (uint32_t index = 0; index < indexHeader.familyCountCount; ++index) {
    FamilyCountHeader record{};
    if (offset > indexFileSize || indexFileSize - offset < sizeof(record) || !indexFile.seekSet64(offset) ||
        !readExact(indexFile, &record, sizeof(record)) || record.urlLength > MAX_URL_BYTES ||
        record.urlLength > indexFileSize - offset - sizeof(record))
      return false;
    candidate.resize(record.urlLength);
    if (record.urlLength && !readExact(indexFile, candidate.data(), candidate.size())) return false;
    offset += sizeof(record) + record.urlLength;
    if (candidate == family) {
      if (UINT32_MAX - count < record.count) return false;
      count += record.count;
      found = true;
    }
  }
  return offset == indexFile.fileSize64();
}

bool lookupBookIndex(HalFile& snapshot, const Header& header, int32_t id, uint64_t& foundOffset, bool& valid) {
  valid = false;
  HalFile indexFile;
  if (!Storage.openFileForRead("BOCat", BOOK_INDEX_LIVE, indexFile)) return false;
  BookIndexHeader indexHeader{};
  if (!readExact(indexFile, &indexHeader, sizeof(indexHeader)) ||
      !bookIndexHeaderMatches(indexHeader, indexFile, snapshot, header))
    return false;
  valid = true;
  uint32_t slot = hashBookId(id) & (indexHeader.capacity - 1);
  const uint64_t fileSize = snapshot.fileSize64();
  for (uint32_t examined = 0; examined < indexHeader.capacity;
       ++examined, slot = (slot + 1) & (indexHeader.capacity - 1)) {
    BookIdSlot value{};
    const uint64_t at = sizeof(BookIndexHeader) + static_cast<uint64_t>(slot) * sizeof(value);
    if (!indexFile.seekSet64(at) || !readExact(indexFile, &value, sizeof(value))) {
      valid = false;
      return false;
    }
    if (value.id == 0) return false;
    if (value.id != id) continue;
    uint8_t kind = 0;
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint64_t next = 0;
    if (!value.rowOffset || !readRecordHeader(snapshot, value.rowOffset, fileSize, kind, descriptor, row, next) ||
        kind != RECORD_ROW || row.role != RowRole::CanonicalBook ||
        row.type != static_cast<uint8_t>(BookOrbitEntryType::BOOK) || row.bookId != id) {
      valid = false;
      return false;
    }
    foundOffset = value.rowOffset;
    return true;
  }
  return false;
}

bool buildBookIndex(HalFile& snapshot, const Header& header) {
  DiskBookIdSet ids(BOOK_INDEX_BUILD, BOOK_INDEX_BUILD_GROW);
  if (!ids.open()) return false;
  uint64_t offset = dataStart(header);
  const uint64_t fileSize = snapshot.fileSize64();
  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(snapshot, offset, fileSize, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.role == RowRole::CanonicalBook) {
      bool alreadyPresent = false;
      uint64_t priorOffset = 0;
      if (!ids.insert(row.bookId, alreadyPresent, priorOffset)) return false;
      if (!alreadyPresent && !ids.setOffset(row.bookId, offset)) return false;
    }
    offset = next;
  }
  uint64_t fingerprint = 0;
  if (offset != fileSize || ids.size() != static_cast<uint32_t>(header.canonicalTotal) ||
      !writeBookIndexTemp(ids, snapshot, header, fingerprint) || !publishBookIndex())
    return false;
  cacheSnapshotFingerprint(header, fileSize, fingerprint);
  return true;
}

bool findCanonicalBook(HalFile& file, const Header& header, int32_t id, uint64_t& foundOffset) {
  bool indexValid = false;
  if (lookupBookIndex(file, header, id, foundOffset, indexValid)) return true;
  if (indexValid) return false;
  if (!buildBookIndex(file, header)) return false;
  return lookupBookIndex(file, header, id, foundOffset, indexValid);
}

bool readDescriptorAt(HalFile& file, uint64_t offset, uint64_t fileSize, Descriptor& result) {
  DescriptorRecord disk{};
  RowRecord row{};
  uint8_t kind = 0;
  uint64_t next = 0;
  if (!readRecordHeader(file, offset, fileSize, kind, disk, row, next) || kind != RECORD_DESCRIPTOR) return false;
  result.offset = offset;
  result.record = disk;
  result.url.resize(disk.urlLength);
  return !disk.urlLength || readExact(file, result.url.data(), disk.urlLength);
}

bool findDescriptor(HalFile& file, const Header& header, uint32_t wanted, Descriptor& result) {
  uint64_t offset = dataStart(header);
  const uint64_t fileSize = file.fileSize64();
  uint32_t index = 0;
  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, fileSize, kind, descriptor, row, next)) return false;
    if (kind == RECORD_DESCRIPTOR) {
      if (index == wanted) return readDescriptorAt(file, offset, fileSize, result);
      ++index;
    }
    offset = next;
  }
  return false;
}

bool scanRow(HalFile& file, uint64_t offset, uint64_t fileSize, RowRecord& row, std::string* fields,
             uint64_t& nextOffset) {
  DescriptorRecord descriptor{};
  uint8_t kind = 0;
  uint64_t next = 0;
  if (!readRecordHeader(file, offset, fileSize, kind, descriptor, row, next) || kind != RECORD_ROW) return false;
  if (fields) {
    if (!file.seekSet64(offset + sizeof(RowRecord))) return false;
    for (size_t i = 0; i < 5; ++i) {
      fields[i].resize(row.fieldLength[i]);
      if (row.fieldLength[i] && !readExact(file, fields[i].data(), row.fieldLength[i])) return false;
    }
  }
  nextOffset = next;
  return true;
}

bool appendDescriptor(HalFile& file, Header& header, const std::string& url, uint8_t listKind, uint32_t& index,
                      bool checkExisting = true) {
  std::string normalizedRequest;
  const std::string* requestUrl = &url;
  if (isBooksListUrl(url)) {
    normalizedRequest = withBookPageSize100(url);
    requestUrl = &normalizedRequest;
  }
  if (requestUrl->size() > MAX_URL_BYTES || !isCatalogHref(*requestUrl) || listKind > LIST_SCOPED_BOOKS) return false;
  if (checkExisting) {
    const std::string canonical = normalizeUrl(*requestUrl);
    uint64_t offset = dataStart(header);
    const uint64_t fileSize = file.fileSize64();
    uint32_t seen = 0;
    while (offset < fileSize) {
      DescriptorRecord disk{};
      RowRecord row{};
      uint8_t kind = 0;
      uint64_t next = 0;
      if (!readRecordHeader(file, offset, fileSize, kind, disk, row, next)) return false;
      if (kind == RECORD_DESCRIPTOR) {
        if (seen >= header.descriptorCount) return false;
        Descriptor prior;
        if (!readDescriptorAt(file, offset, fileSize, prior)) return false;
        if (canonical == normalizeUrl(prior.url)) {
          index = seen;
          if (listKind > disk.canonicalBooks) {
            disk.canonicalBooks = listKind;
            const uint64_t end = file.fileSize64();
            if (!file.seekSet64(offset) || !writeExact(file, &disk, sizeof(disk)) || !file.seekSet64(end)) return false;
          }
          return true;
        }
        ++seen;
      }
      offset = next;
    }
  }
  const uint64_t fileSize = file.fileSize64();
  if (header.descriptorCount == UINT32_MAX) return false;
  DescriptorRecord descriptor{};
  descriptor.magic = DESC_MAGIC;
  descriptor.kind = RECORD_DESCRIPTOR;
  descriptor.canonicalBooks = listKind;
  descriptor.urlLength = static_cast<uint16_t>(requestUrl->size());
  index = header.descriptorCount;
  if (!file.seekSet64(fileSize) || !writeExact(file, &descriptor, sizeof(descriptor)) ||
      !writeExact(file, requestUrl->data(), requestUrl->size()))
    return false;
  ++header.descriptorCount;
  return writeHeader(file, header);
}

bool updateDescriptor(HalFile& file, const Descriptor& descriptor) {
  const uint64_t end = file.fileSize64();
  return file.seekSet64(descriptor.offset) && writeExact(file, &descriptor.record, sizeof(descriptor.record)) &&
         file.seekSet64(end);
}

bool appendRow(HalFile& file, uint32_t owner, bool canonicalBooks, const BookOrbitEntry& entry,
               uint64_t* outOffset = nullptr) {
  std::string normalizedHref;
  const std::string* href = &entry.href;
  if (entry.type == BookOrbitEntryType::NAVIGATION && isBooksListUrl(entry.href)) {
    normalizedHref = withBookPageSize100(entry.href);
    href = &normalizedHref;
  }
  const std::string* fields[] = {&entry.title, &entry.subtitle, href, &entry.series, &entry.formats};
  RowRecord row{};
  row.magic = ROW_MAGIC;
  row.kind = RECORD_ROW;
  row.type = static_cast<uint8_t>(entry.type);
  row.group = static_cast<uint8_t>(entry.group);
  row.hasCover = entry.hasCover ? 1 : 0;
  row.role = canonicalBooks ? RowRole::CanonicalBook : RowRole::Listing;
  row.ownerIndex = owner;
  row.bookId = entry.bookId;
  row.count = entry.count;
  row.progress = entry.progressPercent;
  for (size_t i = 0; i < 5; ++i) row.fieldLength[i] = static_cast<uint16_t>(fields[i]->size());
  const uint64_t offset = file.fileSize64();
  if (!file.seekSet64(offset) || !writeExact(file, &row, sizeof(row))) return false;
  for (const std::string* field : fields)
    if (!writeExact(file, field->data(), field->size())) return false;
  if (outOffset) *outOffset = offset;
  return true;
}

bool decodeRow(HalFile& file, uint64_t offset, uint64_t fileSize, BookOrbitEntry& entry, RowRecord* result);
bool appendShelfReference(HalFile& file, uint32_t rootIndex, const BookOrbitEntry& entry) {
  RowRecord row{};
  row.magic = ROW_MAGIC;
  row.kind = RECORD_ROW;
  row.type = static_cast<uint8_t>(BookOrbitEntryType::BOOK);
  row.group = static_cast<uint8_t>(entry.group);
  row.role = RowRole::ShelfReference;
  row.ownerIndex = rootIndex;
  row.bookId = entry.bookId;
  return file.seekSet64(file.fileSize64()) && writeExact(file, &row, sizeof(row));
}

bool decodeRow(HalFile& file, uint64_t offset, uint64_t fileSize, BookOrbitEntry& entry, RowRecord* result = nullptr) {
  RowRecord row{};
  uint64_t next = 0;
  std::string fields[5];
  if (!scanRow(file, offset, fileSize, row, fields, next)) return false;
  entry = {};
  entry.type = static_cast<BookOrbitEntryType>(row.type);
  entry.group = static_cast<BookOrbitEntryGroup>(row.group);
  entry.hasCover = row.hasCover != 0;
  entry.bookId = row.bookId;
  entry.count = row.count;
  entry.progressPercent = row.progress;
  entry.title = std::move(fields[0]);
  entry.subtitle = std::move(fields[1]);
  entry.href = std::move(fields[2]);
  entry.series = std::move(fields[3]);
  entry.formats = std::move(fields[4]);
  if (result) *result = row;
  return true;
}

bool headerIdentity(const char* path, Header& header) {
  HalFile file;
  return Storage.openFileForRead("BOCat", path, file) && readHeader(file, header, true);
}

bool validateFile(const char* path, bool requireIdentity = true) {
  HalFile file;
  Header header{};
  if (!Storage.openFileForRead("BOCat", path, file) || !readHeader(file, header, requireIdentity)) return false;
  const uint64_t fileSize = file.fileSize64();
  uint64_t offset = dataStart(header);
  uint32_t descriptors = 0, rows = 0, canonicalRows = 0;
  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, fileSize, kind, descriptor, row, next)) return false;
    if (kind == RECORD_DESCRIPTOR) {
      if (!descriptor.complete || descriptors >= header.descriptorCount ||
          descriptor.canonicalBooks > LIST_SCOPED_BOOKS)
        return false;
      Descriptor decoded;
      if (!readDescriptorAt(file, offset, fileSize, decoded) || !isCatalogHref(decoded.url)) return false;
      if ((descriptor.nextIndex != NO_LINK && descriptor.nextIndex >= header.descriptorCount) ||
          (descriptor.prevIndex != NO_LINK && descriptor.prevIndex >= header.descriptorCount))
        return false;
      if (descriptor.canonicalBooks && (descriptor.total < 0 || descriptor.page < 1)) return false;
      ++descriptors;
    } else {
      if (row.ownerIndex >= header.descriptorCount || rows == UINT32_MAX) return false;
      if (row.role == RowRole::ShelfReference) {
        if (row.type != static_cast<uint8_t>(BookOrbitEntryType::BOOK) || row.bookId <= 0 || row.ownerIndex != 0 ||
            (row.group != static_cast<uint8_t>(BookOrbitEntryGroup::CONTINUE_READING) &&
             row.group != static_cast<uint8_t>(BookOrbitEntryGroup::DISCOVER)))
          return false;
        for (const uint16_t length : row.fieldLength)
          if (length != 0) return false;
        bool foundCanonical = false;
        uint64_t candidateOffset = dataStart(header);
        while (candidateOffset < offset) {
          DescriptorRecord candidateDescriptor{};
          RowRecord candidate{};
          uint8_t candidateKind = 0;
          uint64_t candidateNext = 0;
          if (!readRecordHeader(file, candidateOffset, fileSize, candidateKind, candidateDescriptor, candidate,
                                candidateNext))
            return false;
          if (candidateKind == RECORD_ROW && candidate.role == RowRole::CanonicalBook &&
              candidate.bookId == row.bookId) {
            foundCanonical = true;
            break;
          }
          candidateOffset = candidateNext;
        }
        if (!foundCanonical) return false;
      } else if (row.role == RowRole::CanonicalBook) {
        if (row.type != static_cast<uint8_t>(BookOrbitEntryType::BOOK) || row.bookId <= 0 ||
            canonicalRows == UINT32_MAX)
          return false;
        ++canonicalRows;
      }
      ++rows;
    }
    offset = next;
  }
  return offset == fileSize && descriptors == header.descriptorCount && rows == header.rowCount &&
         header.canonicalTotal >= 0 && canonicalRows == static_cast<uint32_t>(header.canonicalTotal);
}

bool recover() {
  const bool liveExists = Storage.exists(LIVE);
  const bool backupExists = Storage.exists(BACKUP);
  if (liveExists && !backupExists) {
    Header header{};
    return headerIdentity(LIVE, header);
  }
  if (liveExists && validateFile(LIVE)) {
    if (backupExists) Storage.remove(BACKUP);
    return true;
  }
  if (backupExists && validateFile(BACKUP)) {
    invalidateSnapshotFingerprint();
    if (liveExists) Storage.remove(LIVE);
    if (Storage.rename(BACKUP, LIVE)) return validateFile(LIVE);
    return false;
  }
  // Keep a valid catalog belonging to a different account inaccessible, not destroyed.
  if (liveExists && !validateFile(LIVE, false)) {
    invalidateSnapshotFingerprint();
    Storage.remove(LIVE);
  }
  if (backupExists && !validateFile(BACKUP, false)) Storage.remove(BACKUP);
  return false;
}

bool fieldContainsFolded(const char* text, size_t length, const std::string& needle) {
  if (needle.empty()) return true;
  if (needle.size() > length) return false;
  for (size_t i = 0; i + needle.size() <= length; ++i) {
    size_t j = 0;
    for (; j < needle.size(); ++j)
      if (std::tolower(static_cast<unsigned char>(text[i + j])) != std::tolower(static_cast<unsigned char>(needle[j])))
        break;
    if (j == needle.size()) return true;
  }
  return false;
}

bool rowMatches(HalFile& file, uint64_t offset, const RowRecord& row, const std::string& query, char* workspace,
                bool& result) {
  result = false;
  if (row.type != static_cast<uint8_t>(BookOrbitEntryType::BOOK) || row.role != RowRole::CanonicalBook) return true;
  uint64_t fieldOffset = offset + sizeof(RowRecord);
  for (size_t i = 0; i < 5; ++i) {
    const size_t length = row.fieldLength[i];
    if (i == 0 || i == 1 || i == 3) {
      if (!file.seekSet64(fieldOffset) || (length && !readExact(file, workspace, length))) return false;
      if (fieldContainsFolded(workspace, length, query)) result = true;
    }
    fieldOffset += length;
  }
  return true;
}

bool scanListing(HalFile& file, const Header& header, uint32_t owner,
                 const BookOrbitCatalogListParser::EntryCallback& callback, uint32_t* count) {
  uint64_t offset = dataStart(header);
  const uint64_t fileSize = file.fileSize64();
  uint32_t found = 0;
  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, fileSize, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.ownerIndex == owner && row.role != RowRole::ShelfReference) {
      if (callback) {
        BookOrbitEntry entry;
        if (!decodeRow(file, offset, fileSize, entry)) return false;
        callback(entry);
      }
      ++found;
    }
    offset = next;
  }
  if (count) *count = found;
  return true;
}

bool canonicalPageNumber(const std::string& url, uint32_t& page) {
  const size_t start = url.find("page=");
  if (start == std::string::npos) {
    page = 1;
    return true;
  }
  const char* value = url.c_str() + start + 5;
  char* end = nullptr;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || (*end != '\0' && *end != '&') || parsed == 0 || parsed > UINT32_MAX) return false;
  page = static_cast<uint32_t>(parsed);
  return true;
}
bool readRowField(HalFile& file, uint64_t offset, const RowRecord& row, size_t field, char* workspace, size_t& length) {
  if (field >= 5 || row.fieldLength[field] >= MAX_FIELD_BYTES) return false;
  uint64_t fieldOffset = offset + sizeof(RowRecord);
  for (size_t index = 0; index < field; ++index) fieldOffset += row.fieldLength[index];
  length = row.fieldLength[field];
  if (!file.seekSet64(fieldOffset) || (length && !readExact(file, workspace, length))) return false;
  workspace[length] = '\0';
  return true;
}

size_t seriesNameLength(const char* text, size_t length) {
  size_t marker = length;
  for (size_t index = 0; index + 2 <= length; ++index)
    if (text[index] == ' ' && text[index + 1] == '#') marker = index;
  if (marker == length || marker + 2 == length) return length;
  bool hasDigit = false;
  for (size_t index = marker + 2; index < length; ++index) {
    const char value = text[index];
    if (value >= '0' && value <= '9')
      hasDigit = true;
    else if (value != '.')
      return length;
  }
  return hasDigit ? marker : length;
}
bool containsAuthorToken(const char* text, size_t length, const std::string& label) {
  static constexpr char SEPARATOR[] = " \xC2\xB7 ";
  constexpr size_t SEPARATOR_LENGTH = sizeof(SEPARATOR) - 1;
  size_t start = 0;
  while (start <= length) {
    size_t end = length;
    for (size_t candidate = start; candidate + SEPARATOR_LENGTH <= length; ++candidate) {
      if (memcmp(text + candidate, SEPARATOR, SEPARATOR_LENGTH) == 0) {
        end = candidate;
        break;
      }
    }
    size_t left = start;
    size_t right = end;
    while (left < right && std::isspace(static_cast<unsigned char>(text[left]))) ++left;
    while (right > left && std::isspace(static_cast<unsigned char>(text[right - 1]))) --right;
    if (right - left == label.size() && memcmp(text + left, label.data(), label.size()) == 0) return true;
    if (end + SEPARATOR_LENGTH > length) break;
    start = end + SEPARATOR_LENGTH;
  }
  return false;
}

bool scanUniqueLabels(HalFile& file, const Header& header, bool authors, uint64_t begin, uint64_t end,
                      const BookOrbitCatalogListParser::EntryCallback& callback, uint32_t& total) {
  auto labelBuffer = makeUniqueNoThrow<char[]>(MAX_FIELD_BYTES);
  if (!labelBuffer) {
    LOG_ERR("BOCat", "Offline navigation workspace allocation failed");
    return false;
  }
  Storage.remove(BOOK_LABEL_DATA_WORK);
  struct RemoveLabelData {
    ~RemoveLabelData() { Storage.remove(BOOK_LABEL_DATA_WORK); }
  } cleanup;
  HalFile labelData;
  HalFile labelCreate;
  DiskBookIdSet labels(BOOK_LABEL_SET_WORK, BOOK_LABEL_SET_GROW);
  if (!Storage.openFileForWrite("BOCat", BOOK_LABEL_DATA_WORK, labelCreate) || !labelCreate.close() ||
      !Storage.openFileForUpdate("BOCat", BOOK_LABEL_DATA_WORK, labelData) || !labels.open())
    return false;

  static constexpr char AUTHOR_SEPARATOR[] = " \xC2\xB7 ";
  constexpr size_t AUTHOR_SEPARATOR_LENGTH = sizeof(AUTHOR_SEPARATOR) - 1;
  const size_t field = authors ? 1 : 3;
  const uint64_t fileSize = file.fileSize64();
  uint64_t offset = dataStart(header);
  uint32_t index = 0;
  std::string label;
  label.reserve(MAX_FIELD_BYTES - 1);
  uint8_t priorBytes[64];
  auto rememberLabel = [&](bool& inserted) {
    if (label.empty() || label.size() >= MAX_FIELD_BYTES) return false;
    uint32_t hash = 2166136261U;
    for (const unsigned char byte : label) hash = (hash ^ byte) * 16777619U;
    uint32_t key = hash & INT32_MAX;
    if (key == 0) key = 1;
    for (uint32_t attempt = 0; attempt < static_cast<uint32_t>(INT32_MAX); ++attempt) {
      bool alreadyPresent = false;
      uint64_t priorOffset = 0;
      if (!labels.insert(static_cast<int32_t>(key), alreadyPresent, priorOffset)) return false;
      if (!alreadyPresent) {
        const uint64_t labelOffset = labelData.fileSize64();
        const uint16_t labelLength = static_cast<uint16_t>(label.size());
        if (!labelData.seekSet64(labelOffset) || !writeExact(labelData, &labelLength, sizeof(labelLength)) ||
            !writeExact(labelData, label.data(), label.size()) ||
            !labels.setOffset(static_cast<int32_t>(key), labelOffset))
          return false;
        inserted = true;
        return true;
      }
      uint16_t priorLength = 0;
      if (!labelData.seekSet64(priorOffset) || !readExact(labelData, &priorLength, sizeof(priorLength)) ||
          priorLength >= MAX_FIELD_BYTES)
        return false;
      bool equal = priorLength == label.size();
      size_t compared = 0;
      while (equal && compared < priorLength) {
        const size_t bytes = std::min(sizeof(priorBytes), static_cast<size_t>(priorLength) - compared);
        if (!readExact(labelData, priorBytes, bytes)) return false;
        equal = memcmp(priorBytes, label.data() + compared, bytes) == 0;
        compared += bytes;
      }
      if (equal) {
        inserted = false;
        return true;
      }
      key = key == static_cast<uint32_t>(INT32_MAX) ? 1 : key + 1;
    }
    return false;
  };

  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, fileSize, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.role == RowRole::CanonicalBook &&
        row.type == static_cast<uint8_t>(BookOrbitEntryType::BOOK)) {
      size_t length = 0;
      if (!readRowField(file, offset, row, field, labelBuffer.get(), length)) return false;
      if (!authors) length = seriesNameLength(labelBuffer.get(), length);
      size_t start = 0;
      do {
        size_t finish = length;
        if (authors) {
          for (size_t candidate = start; candidate + AUTHOR_SEPARATOR_LENGTH <= length; ++candidate) {
            if (memcmp(labelBuffer.get() + candidate, AUTHOR_SEPARATOR, AUTHOR_SEPARATOR_LENGTH) == 0) {
              finish = candidate;
              break;
            }
          }
        }
        size_t left = start;
        size_t right = finish;
        while (left < right && std::isspace(static_cast<unsigned char>(labelBuffer[left]))) ++left;
        while (right > left && std::isspace(static_cast<unsigned char>(labelBuffer[right - 1]))) --right;
        if (right > left) {
          label.assign(labelBuffer.get() + left, right - left);
          bool inserted = false;
          if (!rememberLabel(inserted)) return false;
          if (inserted) {
            if (callback && index >= begin && index < end) {
              BookOrbitEntry entry;
              entry.type = BookOrbitEntryType::NAVIGATION;
              entry.title = label;
              entry.href = localPageUrl(authors ? "author" : "series-books", 1, &label);
              callback(entry);
            }
            if (index != UINT32_MAX) ++index;
          }
        }
        if (!authors || finish == length) break;
        start = finish + AUTHOR_SEPARATOR_LENGTH;
      } while (start <= length);
    }
    offset = next;
  }
  total = index;
  return true;
}

bool localBookMatches(HalFile& file, const RowRecord& row, uint64_t offset, bool authors, const std::string& label,
                      char* workspace, bool& matches) {
  matches = false;
  if (row.role != RowRole::CanonicalBook || row.type != static_cast<uint8_t>(BookOrbitEntryType::BOOK)) return true;
  size_t length = 0;
  if (!readRowField(file, offset, row, authors ? 1 : 3, workspace, length)) return false;
  if (!authors) length = seriesNameLength(workspace, length);
  matches = authors ? containsAuthorToken(workspace, length, label)
                    : length == label.size() && memcmp(workspace, label.data(), length) == 0;
  return true;
}

bool scanLocalBooks(HalFile& file, const Header& header, bool authors, const std::string& label, uint64_t begin,
                    uint64_t end, const BookOrbitCatalogListParser::EntryCallback& callback, uint32_t& total) {
  if (label.size() >= MAX_FIELD_BYTES) return false;
  // Reuse one field-sized buffer for each row; no per-book scratch allocation.
  auto workspace = makeUniqueNoThrow<char[]>(MAX_FIELD_BYTES);
  if (!workspace) {
    LOG_ERR("BOCat", "Offline navigation workspace allocation failed");
    return false;
  }
  const uint64_t fileSize = file.fileSize64();
  uint64_t offset = dataStart(header);
  uint32_t index = 0;
  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, fileSize, kind, descriptor, row, next)) return false;
    bool matches = false;
    if (kind == RECORD_ROW && !localBookMatches(file, row, offset, authors, label, workspace.get(), matches))
      return false;
    if (matches) {
      if (callback && index >= begin && index < end) {
        BookOrbitEntry entry;
        if (!decodeRow(file, offset, fileSize, entry)) return false;
        callback(entry);
      }
      if (index != UINT32_MAX) ++index;
    }
    offset = next;
  }
  total = index;
  return true;
}

bool localListing(HalFile& file, const Header& header, const std::string& url,
                  const BookOrbitCatalogListParser::EntryCallback& callback, BookOrbitPageInfo& outPage) {
  uint32_t page = 0;
  std::string label;
  bool authors = false;
  bool labelsListing = localRoute(url, "authors", page) || localRoute(url, "series", page);
  if (labelsListing) authors = url.compare(0, strlen("@offline:authors:"), "@offline:authors:") == 0;
  if (!labelsListing && isAggregateSection(url, authors)) {
    page = 1;
    labelsListing = true;
  }
  if (labelsListing) {
    const uint64_t begin = static_cast<uint64_t>(page - 1) * LOCAL_PAGE_SIZE;
    const uint64_t end = begin + LOCAL_PAGE_SIZE;
    uint32_t total = 0;
    if (!scanUniqueLabels(file, header, authors, begin, end, callback, total)) return false;
    outPage = {};
    outPage.total = total > INT32_MAX ? INT32_MAX : static_cast<int32_t>(total);
    outPage.page = static_cast<int32_t>(page);
    if (page > 1) outPage.prevUrl = localPageUrl(authors ? "authors" : "series", page - 1);
    if (end < total) outPage.nextUrl = localPageUrl(authors ? "authors" : "series", page + 1);
    return true;
  }

  if (localRoute(url, "author", page, &label)) {
    authors = true;
  } else if (localRoute(url, "series-books", page, &label)) {
    authors = false;
  } else {
    return false;
  }
  const uint64_t begin = static_cast<uint64_t>(page - 1) * BOOK_PAGE_SIZE;
  const uint64_t end = begin + BOOK_PAGE_SIZE;
  uint32_t total = 0;
  if (!scanLocalBooks(file, header, authors, label, begin, end, callback, total)) return false;
  outPage = {};
  outPage.total = total > INT32_MAX ? INT32_MAX : static_cast<int32_t>(total);
  outPage.page = static_cast<int32_t>(page);
  if (page > 1) outPage.prevUrl = localPageUrl(authors ? "author" : "series-books", page - 1, &label);
  if (end < total) outPage.nextUrl = localPageUrl(authors ? "author" : "series-books", page + 1, &label);
  return true;
}

bool writeBookIndexTemp(DiskBookIdSet& ids, HalFile& snapshot, const Header& header, uint64_t& fingerprint) {
  Storage.remove(BOOK_INDEX_TEMP);
  Storage.remove(BOOK_INDEX_COUNTS_WORK);
  HalFile index;
  HalFile counts;
  if (!computeSnapshotFingerprint(snapshot, fingerprint)) return false;
  BookIndexHeader indexHeader = makeBookIndexHeader(snapshot, header, ids.slotCount(), fingerprint);
  const uint64_t familyOffset =
      sizeof(BookIndexHeader) + static_cast<uint64_t>(indexHeader.capacity) * sizeof(BookIdSlot);
  auto fail = [&]() {
    if (index) index.close();
    if (counts) counts.close();
    Storage.remove(BOOK_INDEX_TEMP);
    Storage.remove(BOOK_INDEX_COUNTS_WORK);
    return false;
  };
  if (familyOffset > UINT32_MAX || !Storage.openFileForWrite("BOCat", BOOK_INDEX_TEMP, index) ||
      !writeExact(index, &indexHeader, sizeof(indexHeader)) || !ids.copyTo(index, sizeof(indexHeader)) ||
      !Storage.openFileForWrite("BOCat", BOOK_INDEX_COUNTS_WORK, counts))
    return fail();

  uint32_t zeroes[32]{};
  uint64_t remaining = static_cast<uint64_t>(header.descriptorCount) * sizeof(uint32_t);
  while (remaining) {
    const size_t bytes = remaining < sizeof(zeroes) ? static_cast<size_t>(remaining) : sizeof(zeroes);
    if (!writeExact(counts, zeroes, bytes)) return fail();
    remaining -= bytes;
  }
  auto addDescriptorCount = [&](uint32_t owner, uint32_t amount) {
    uint32_t count = 0;
    const uint64_t at = static_cast<uint64_t>(owner) * sizeof(count);
    if (!counts.seekSet64(at) || !readExact(counts, &count, sizeof(count)) || UINT32_MAX - count < amount) return false;
    count += amount;
    return counts.seekSet64(at) && writeExact(counts, &count, sizeof(count));
  };
  uint64_t offset = dataStart(header);
  const uint64_t fileSize = snapshot.fileSize64();
  uint32_t currentOwner = NO_LINK;
  uint32_t currentCount = 0;
  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(snapshot, offset, fileSize, kind, descriptor, row, next)) return fail();
    if (kind == RECORD_ROW && row.role != RowRole::ShelfReference) {
      if (row.ownerIndex >= header.descriptorCount) return fail();
      if (row.ownerIndex != currentOwner) {
        if (currentOwner != NO_LINK && !addDescriptorCount(currentOwner, currentCount)) return fail();
        currentOwner = row.ownerIndex;
        currentCount = 0;
      }
      if (currentCount == UINT32_MAX) return fail();
      ++currentCount;
    }
    offset = next;
  }
  if (offset != fileSize || (currentOwner != NO_LINK && !addDescriptorCount(currentOwner, currentCount))) return fail();

  offset = dataStart(header);
  uint32_t descriptorIndex = 0;
  while (offset < fileSize) {
    DescriptorRecord descriptorRecord{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(snapshot, offset, fileSize, kind, descriptorRecord, row, next)) return fail();
    if (kind == RECORD_DESCRIPTOR) {
      Descriptor descriptor;
      if (descriptorIndex >= header.descriptorCount || !readDescriptorAt(snapshot, offset, fileSize, descriptor))
        return fail();
      if (descriptor.record.complete) {
        const std::string family = pageFamilyUrl(descriptor.url);
        uint32_t count = 0;
        const uint64_t countOffset = static_cast<uint64_t>(descriptorIndex) * sizeof(count);
        if (family.size() > MAX_URL_BYTES || !counts.seekSet64(countOffset) ||
            !readExact(counts, &count, sizeof(count)))
          return fail();
        const FamilyCountHeader familyCount{static_cast<uint16_t>(family.size()), 0, count};
        if (!writeExact(index, &familyCount, sizeof(familyCount)) || !writeExact(index, family.data(), family.size()))
          return fail();
        ++indexHeader.familyCountCount;
      }
      ++descriptorIndex;
    }
    offset = next;
  }
  if (offset != fileSize || descriptorIndex != header.descriptorCount || !counts.close()) return fail();
  Storage.remove(BOOK_INDEX_COUNTS_WORK);
  indexHeader.familyCountOffset = static_cast<uint32_t>(familyOffset);
  if (!index.seekSet64(0) || !writeExact(index, &indexHeader, sizeof(indexHeader))) return fail();
  index.flush();
  if (!index.close()) {
    Storage.remove(BOOK_INDEX_TEMP);
    Storage.remove(BOOK_INDEX_COUNTS_WORK);
    return false;
  }
  return true;
}
bool ensureBookIndex(HalFile& snapshot, const Header& header) {
  HalFile indexFile;
  BookIndexHeader indexHeader{};
  return readBookIndexHeader(snapshot, header, indexHeader, indexFile) || buildBookIndex(snapshot, header);
}

bool ensureFamilyCount(HalFile& snapshot, const Header& header, const std::string& family, uint32_t& count,
                       bool& found) {
  if (!ensureBookIndex(snapshot, header)) return false;
  if (lookupFamilyCount(snapshot, header, family, count, found)) return true;
  return buildBookIndex(snapshot, header) && lookupFamilyCount(snapshot, header, family, count, found);
}

uint32_t familyListingCount(HalFile& file, const Header& header, const std::string& family, bool& ok) {
  uint32_t count = 0;
  bool found = false;
  ok = ensureFamilyCount(file, header, family, count, found);
  return ok && found ? count : 0;
}
bool sourceListingCount(HalFile& file, const Header& header, const std::string& url, uint32_t& count, bool& found) {
  found = false;
  count = 0;
  return ensureFamilyCount(file, header, pageFamilyUrl(url), count, found);
}

enum class RebuildPhase : uint8_t { Idle, Snapshot, Finalize, Complete, Failed, Cancelled };

struct RebuildState {
  RebuildPhase phase = RebuildPhase::Idle;
  BookOrbitCatalogClient::Error error = BookOrbitCatalogClient::JSON_ERROR;
  BookOrbitCatalogSettings settings;
  DiskBookIdSet bookIds;
  // Source-map reserved stores descriptor index + 1; rowOffset stores the
  // source-directory row offset.
  DiskBookIdSet librarySourceIds{SOURCE_LIBRARY_IDS, SOURCE_LIBRARY_IDS_GROW};
  DiskBookIdSet collectionSourceIds{SOURCE_COLLECTION_IDS, SOURCE_COLLECTION_IDS_GROW};
  DiskBookIdSet smartScopeSourceIds{SOURCE_SMART_SCOPE_IDS, SOURCE_SMART_SCOPE_IDS_GROW};
  HalFile file;
  Header header{};
  uint32_t cursor = 0;
  uint32_t generationStartMs = 0;
  uint32_t stageStartMs = 0;
  uint64_t sourceFingerprint = 0;
  std::string query;
  std::string requestQuery;
  std::string cursorToken;
  std::string generation;
  std::string etag;
  int32_t snapshotTotal = -1;
  uint32_t expectedPages = 0;
  uint32_t pagesFetched = 0;
  uint32_t booksFetched = 0;
  uint32_t booksOnPage = 0;
  int32_t currentBookId = 0;
  bool currentBookHasSelectedMembership = false;
  const BookOrbitEntry* currentBookEntry = nullptr;
  uint32_t rootIndex = 0;
  uint32_t allBooksIndex = 0;
  uint32_t sourceSectionIndices[3]{};
  uint8_t continueCount = 0;
  uint8_t discoverCount = 0;
  int32_t seenContinue[5]{};
  int32_t seenDiscover[12]{};
  const char* stage = "idle";
};
RebuildState rebuildState;
void closeSourceIdSets(RebuildState& state);

void finishGenerationStage(const char* nextStage) {
  const uint32_t now = millis();
  LOG_INF("BOCat", "Generation stage=%s ms=%lu", rebuildState.stage,
          static_cast<unsigned long>(now - rebuildState.stageStartMs));
  rebuildState.stage = nextStage;
  rebuildState.stageStartMs = now;
}

BookOrbitCatalogStore::RebuildStatus failGeneration(const char* failure, BookOrbitCatalogClient::Error error) {
  const uint32_t now = millis();
  LOG_ERR("BOCat", "Generation failed at %s: stage=%s stage_ms=%lu total_ms=%lu error=%d HTTP=%d", failure,
          rebuildState.stage, static_cast<unsigned long>(now - rebuildState.stageStartMs),
          static_cast<unsigned long>(now - rebuildState.generationStartMs), static_cast<int>(error),
          BookOrbitCatalogClient::lastHttpCode);
  if (rebuildState.file) rebuildState.file.close();
  rebuildState.bookIds.close();
  closeSourceIdSets(rebuildState);
  rebuildState.currentBookEntry = nullptr;
  Storage.remove(TEMP);
  Storage.remove(BOOK_INDEX_TEMP);
  Storage.remove(BOOK_INDEX_COUNTS_WORK);
  rebuildState.error = error;
  rebuildState.phase = RebuildPhase::Failed;
  return BookOrbitCatalogStore::RebuildStatus::Failed;
}

void fillRebuildProgress(BookOrbitCatalogStore::RebuildProgress* progress) {
  if (!progress) return;
  progress->completedPages = rebuildState.pagesFetched;
  progress->discoveredPages = rebuildState.expectedPages;
}

int sourceSlot(BookOrbitCatalogSettings::SourceKind kind) {
  switch (kind) {
    case BookOrbitCatalogSettings::SourceKind::Library:
      return 0;
    case BookOrbitCatalogSettings::SourceKind::Collection:
      return 1;
    case BookOrbitCatalogSettings::SourceKind::SmartScope:
      return 2;
  }
  return -1;
}

DiskBookIdSet* sourceIdSet(RebuildState& state, int slot) {
  switch (slot) {
    case 0:
      return &state.librarySourceIds;
    case 1:
      return &state.collectionSourceIds;
    case 2:
      return &state.smartScopeSourceIds;
  }
  return nullptr;
}

void closeSourceIdSets(RebuildState& state) {
  state.librarySourceIds.close();
  state.collectionSourceIds.close();
  state.smartScopeSourceIds.close();
}

void removeSourceIdSetFiles() {
  Storage.remove(SOURCE_LIBRARY_IDS);
  Storage.remove(SOURCE_LIBRARY_IDS_GROW);
  Storage.remove(SOURCE_COLLECTION_IDS);
  Storage.remove(SOURCE_COLLECTION_IDS_GROW);
  Storage.remove(SOURCE_SMART_SCOPE_IDS);
  Storage.remove(SOURCE_SMART_SCOPE_IDS_GROW);
}

bool openSourceIdSets(RebuildState& state) {
  return state.librarySourceIds.open() && state.collectionSourceIds.open() && state.smartScopeSourceIds.open();
}

const char* sourceSectionPath(BookOrbitCatalogSettings::SourceKind kind) {
  switch (kind) {
    case BookOrbitCatalogSettings::SourceKind::Library:
      return "sections/libraries";
    case BookOrbitCatalogSettings::SourceKind::Collection:
      return "sections/collections";
    case BookOrbitCatalogSettings::SourceKind::SmartScope:
      return "sections/smart-scopes";
  }
  return "";
}

const char* sourceSectionTitle(BookOrbitCatalogSettings::SourceKind kind) {
  switch (kind) {
    case BookOrbitCatalogSettings::SourceKind::Library:
      return tr(STR_BOOKORBIT_LIBRARIES);
    case BookOrbitCatalogSettings::SourceKind::Collection:
      return tr(STR_BOOKORBIT_COLLECTIONS);
    case BookOrbitCatalogSettings::SourceKind::SmartScope:
      return tr(STR_BOOKORBIT_SMART_SCOPES);
  }
  return "";
}

const char* snapshotSourceQueryKey(BookOrbitCatalogSettings::SourceKind kind) {
  switch (kind) {
    case BookOrbitCatalogSettings::SourceKind::Library:
      return "libraryIds";
    case BookOrbitCatalogSettings::SourceKind::Collection:
      return "collectionIds";
    case BookOrbitCatalogSettings::SourceKind::SmartScope:
      return "smartScopeIds";
  }
  return "";
}

bool buildSnapshotQuery(const BookOrbitCatalogSettings& settings, std::string& query) {
  const char* scope = nullptr;
  switch (settings.scope) {
    case BookOrbitCatalogSettings::Scope::All:
      scope = "all";
      break;
    case BookOrbitCatalogSettings::Scope::Selected:
      scope = "selected";
      break;
  }
  if (!scope || settings.sources.size() > BookOrbitCatalogSettings::MAX_SOURCES) return false;

  query.clear();
  query.reserve(MAX_URL_BYTES);
  query = "scope=";
  query += scope;
  query += "&size=100";
  if (settings.supportedOnly) query += "&formats=epub,xtc,xtch,txt,md";
  if (settings.scope == BookOrbitCatalogSettings::Scope::Selected) {
    static constexpr BookOrbitCatalogSettings::SourceKind KINDS[] = {
        BookOrbitCatalogSettings::SourceKind::Library,
        BookOrbitCatalogSettings::SourceKind::Collection,
        BookOrbitCatalogSettings::SourceKind::SmartScope,
    };
    for (const auto& source : settings.sources)
      if (sourceSlot(source.kind) < 0 || source.id <= 0) return false;
    for (const auto kind : KINDS) {
      bool firstId = true;
      for (size_t index = 0; index < settings.sources.size(); ++index) {
        const auto& source = settings.sources[index];
        if (source.kind != kind) continue;
        for (size_t prior = 0; prior < index; ++prior)
          if (settings.sources[prior].kind == kind && settings.sources[prior].id == source.id) return false;
        if (firstId) {
          query.push_back('&');
          query += snapshotSourceQueryKey(kind);
          query.push_back('=');
          firstId = false;
        } else {
          query.push_back(',');
        }
        query += std::to_string(source.id);
      }
    }
  }
  return query.size() <= MAX_URL_BYTES;
}

bool validSnapshotEntryFields(const BookOrbitEntry& entry) {
  return entry.title.size() < MAX_FIELD_BYTES && entry.subtitle.size() < MAX_FIELD_BYTES &&
         entry.href.size() < MAX_FIELD_BYTES && entry.series.size() < MAX_FIELD_BYTES &&
         entry.formats.size() < MAX_FIELD_BYTES;
}

bool addStagedRow(RebuildState& state, uint32_t owner, bool canonical, const BookOrbitEntry& entry,
                  uint64_t* canonicalOffset = nullptr) {
  if (state.header.rowCount == UINT32_MAX || !validSnapshotEntryFields(entry) ||
      !appendRow(state.file, owner, canonical, entry, canonicalOffset))
    return false;
  ++state.header.rowCount;
  return true;
}

bool appendInitialNavigation(RebuildState& state) {
  if (!appendDescriptor(state.file, state.header, "", LIST_NORMAL, state.rootIndex) || state.rootIndex != 0 ||
      !appendDescriptor(state.file, state.header, "books?sort=title&size=100", LIST_ALL_BOOKS, state.allBooksIndex))
    return false;

  static constexpr BookOrbitCatalogSettings::SourceKind KINDS[] = {
      BookOrbitCatalogSettings::SourceKind::Library,
      BookOrbitCatalogSettings::SourceKind::Collection,
      BookOrbitCatalogSettings::SourceKind::SmartScope,
  };
  for (const auto kind : KINDS) {
    const int slot = sourceSlot(kind);
    if (slot < 0 || !appendDescriptor(state.file, state.header, sourceSectionPath(kind), LIST_NORMAL,
                                      state.sourceSectionIndices[slot]))
      return false;
  }
  uint32_t ignored = 0;
  if (!appendDescriptor(state.file, state.header, "sections/authors", LIST_NORMAL, ignored) ||
      !appendDescriptor(state.file, state.header, "sections/series", LIST_NORMAL, ignored))
    return false;

  for (const auto kind : KINDS) {
    BookOrbitEntry entry;
    entry.title = sourceSectionTitle(kind);
    entry.href = sourceSectionPath(kind);
    if (!addStagedRow(state, state.rootIndex, false, entry)) return false;
  }
  BookOrbitEntry authors;
  authors.title = tr(STR_TAB_AUTHORS);
  authors.href = "sections/authors";
  BookOrbitEntry series;
  series.title = tr(STR_BOOKORBIT_SERIES);
  series.href = "sections/series";
  return addStagedRow(state, state.rootIndex, false, authors) && addStagedRow(state, state.rootIndex, false, series);
}

bool ensureSourceDescriptor(RebuildState& state, BookOrbitCatalogSettings::SourceKind kind, int32_t id,
                            uint32_t& owner) {
  const int slot = sourceSlot(kind);
  DiskBookIdSet* ids = sourceIdSet(state, slot);
  if (id <= 0 || !ids) return false;
  uint32_t encodedOwner = 0;
  uint64_t metadataOffset = 0;
  bool found = false;
  if (!ids->lookupValues(id, encodedOwner, metadataOffset, found)) return false;
  if (found) {
    if (encodedOwner == 0) return false;
    owner = encodedOwner - 1;
    return true;
  }

  bool alreadyPresent = false;
  uint64_t ignoredOffset = 0;
  if (!ids->insert(id, alreadyPresent, ignoredOffset) || alreadyPresent ||
      !appendDescriptor(state.file, state.header, sourceBooksUrl(kind, id), LIST_SCOPED_BOOKS, owner, false))
    return false;
  if (owner == UINT32_MAX) return false;
  return ids->setReserved(id, owner + 1);
}

bool decodeSourceDirectoryRecord(HalFile& file, uint64_t offset, uint32_t owner, const std::string& href,
                                 BookOrbitEntry& entry) {
  RowRecord row{};
  return decodeRow(file, offset, file.fileSize64(), entry, &row) && row.ownerIndex == owner &&
         entry.type == BookOrbitEntryType::NAVIGATION && entry.href == href && entry.count >= 0;
}

bool snapshotBook(void* context, const BookOrbitEntry& sourceEntry) {
  auto& state = *static_cast<RebuildState*>(context);
  if (sourceEntry.type != BookOrbitEntryType::BOOK || sourceEntry.bookId <= 0 ||
      sourceEntry.href != "books/" + std::to_string(sourceEntry.bookId) ||
      (state.settings.supportedOnly && !BookOrbitCatalogSettings::supportsFormats(sourceEntry.formats)) ||
      state.booksFetched == UINT32_MAX || state.booksOnPage == UINT32_MAX ||
      (state.currentBookId > 0 && state.settings.scope == BookOrbitCatalogSettings::Scope::Selected &&
       !state.currentBookHasSelectedMembership))
    return false;
  bool alreadyPresent = false;
  uint64_t existingOffset = 0;
  if (!state.bookIds.insert(sourceEntry.bookId, alreadyPresent, existingOffset) || alreadyPresent) return false;
  BookOrbitEntry entry = sourceEntry;
  entry.group = BookOrbitEntryGroup::STANDARD;
  uint64_t rowOffset = 0;
  if (!addStagedRow(state, state.allBooksIndex, true, entry, &rowOffset) ||
      !state.bookIds.setOffset(sourceEntry.bookId, rowOffset))
    return false;
  state.currentBookId = sourceEntry.bookId;
  state.currentBookHasSelectedMembership = false;
  state.currentBookEntry = &sourceEntry;
  ++state.booksOnPage;
  return true;
}

bool snapshotSource(void* context, BookOrbitCatalogSettings::SourceKind kind, int32_t id, const std::string& name,
                    int32_t bookCount) {
  auto& state = *static_cast<RebuildState*>(context);
  const int slot = sourceSlot(kind);
  if (slot < 0 || id <= 0 || bookCount < 0 || name.size() >= MAX_FIELD_BYTES) return false;
  if (state.settings.scope == BookOrbitCatalogSettings::Scope::Selected && !selectedSource(state.settings, kind, id))
    return true;

  DiskBookIdSet* ids = sourceIdSet(state, slot);
  if (!ids) return false;
  uint32_t encodedOwner = 0;
  uint64_t metadataOffset = 0;
  bool found = false;
  if (!ids->lookupValues(id, encodedOwner, metadataOffset, found)) return false;
  if (state.pagesFetched > 0 && (!found || encodedOwner == 0 || metadataOffset == 0)) return false;

  uint32_t sourceOwner = 0;
  if (!ensureSourceDescriptor(state, kind, id, sourceOwner) ||
      !ids->lookupValues(id, encodedOwner, metadataOffset, found) || !found || encodedOwner != sourceOwner + 1)
    return false;
  BookOrbitEntry expected;
  expected.title = name;
  expected.href = sourceBooksUrl(kind, id);
  expected.count = bookCount;
  if (!validSnapshotEntryFields(expected)) return false;

  if (metadataOffset) {
    BookOrbitEntry existing;
    return decodeSourceDirectoryRecord(state.file, metadataOffset, state.sourceSectionIndices[slot], expected.href,
                                       existing) &&
           existing.title == expected.title && existing.count == expected.count;
  }
  if (state.pagesFetched > 0) return false;
  uint64_t rowOffset = 0;
  if (!addStagedRow(state, state.sourceSectionIndices[slot], false, expected, &rowOffset) || rowOffset == 0)
    return false;
  return ids->setOffset(id, rowOffset);
}

bool snapshotMembership(void* context, int32_t bookId, BookOrbitCatalogSettings::SourceKind kind, int32_t sourceId) {
  auto& state = *static_cast<RebuildState*>(context);
  if (bookId <= 0 || sourceId <= 0 || sourceSlot(kind) < 0 || bookId != state.currentBookId ||
      !state.currentBookEntry || state.currentBookEntry->bookId != bookId)
    return false;
  if (state.settings.scope == BookOrbitCatalogSettings::Scope::Selected &&
      !selectedSource(state.settings, kind, sourceId))
    return true;
  if (state.settings.scope == BookOrbitCatalogSettings::Scope::Selected) state.currentBookHasSelectedMembership = true;
  uint32_t owner = 0;
  return ensureSourceDescriptor(state, kind, sourceId, owner) &&
         addStagedRow(state, owner, false, *state.currentBookEntry);
}

bool snapshotShelf(void* context, BookOrbitEntryGroup group, int32_t bookId) {
  auto& state = *static_cast<RebuildState*>(context);
  if (bookId <= 0) return false;
  int32_t* seen = nullptr;
  uint8_t* count = nullptr;
  switch (group) {
    case BookOrbitEntryGroup::CONTINUE_READING:
      seen = state.seenContinue;
      count = &state.continueCount;
      break;
    case BookOrbitEntryGroup::DISCOVER:
      seen = state.seenDiscover;
      count = &state.discoverCount;
      break;
    case BookOrbitEntryGroup::STANDARD:
      return false;
  }
  for (uint8_t index = 0; index < *count; ++index)
    if (seen[index] == bookId) return true;
  const uint8_t maximum = group == BookOrbitEntryGroup::CONTINUE_READING ? 5 : 12;
  if (*count >= maximum) return false;
  seen[(*count)++] = bookId;
  return true;
}

bool appendSnapshotShelves(RebuildState& state) {
  const auto appendGroup = [&](const int32_t* ids, uint8_t count, BookOrbitEntryGroup group) {
    for (uint8_t index = 0; index < count; ++index) {
      uint64_t offset = 0;
      if (!state.bookIds.lookup(ids[index], offset)) continue;
      BookOrbitEntry shelf;
      shelf.bookId = ids[index];
      shelf.group = group;
      if (state.header.rowCount == UINT32_MAX || !appendShelfReference(state.file, state.rootIndex, shelf))
        return false;
      ++state.header.rowCount;
    }
    return true;
  };
  return appendGroup(state.seenContinue, state.continueCount, BookOrbitEntryGroup::CONTINUE_READING) &&
         appendGroup(state.seenDiscover, state.discoverCount, BookOrbitEntryGroup::DISCOVER);
}

bool validateSnapshotSources(RebuildState& state) {
  uint64_t offset = dataStart(state.header);
  const uint64_t size = state.file.fileSize64();
  uint32_t descriptorIndex = 0;
  while (offset < size) {
    DescriptorRecord disk{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(state.file, offset, size, kind, disk, row, next)) return false;
    if (kind == RECORD_DESCRIPTOR) {
      if (descriptorIndex >= state.header.descriptorCount) return false;
      if (disk.canonicalBooks == LIST_SCOPED_BOOKS) {
        Descriptor descriptor;
        BookOrbitCatalogSettings::SourceKind sourceKind{};
        int32_t sourceId = 0;
        if (!readDescriptorAt(state.file, offset, size, descriptor) ||
            !sourceForHref(descriptor.url, sourceKind, sourceId))
          return false;
        const int slot = sourceSlot(sourceKind);
        DiskBookIdSet* ids = sourceIdSet(state, slot);
        uint32_t encodedOwner = 0;
        uint64_t metadataOffset = 0;
        bool found = false;
        BookOrbitEntry source;
        if (!ids || !ids->lookupValues(sourceId, encodedOwner, metadataOffset, found) || !found ||
            encodedOwner != descriptorIndex + 1 || metadataOffset == 0 ||
            !decodeSourceDirectoryRecord(state.file, metadataOffset, state.sourceSectionIndices[slot],
                                         sourceBooksUrl(sourceKind, sourceId), source))
          return false;
      }
      ++descriptorIndex;
    }
    offset = next;
  }
  return offset == size && descriptorIndex == state.header.descriptorCount;
}

bool completeSnapshotDescriptors(RebuildState& state) {
  Storage.remove(BOOK_INDEX_COUNTS_WORK);
  HalFile counts;
  if (!Storage.openFileForWrite("BOCat", BOOK_INDEX_COUNTS_WORK, counts)) return false;
  uint32_t zeroes[32]{};
  uint64_t remaining = static_cast<uint64_t>(state.header.descriptorCount) * sizeof(uint32_t);
  while (remaining) {
    const size_t bytes = std::min<uint64_t>(remaining, sizeof(zeroes));
    if (!writeExact(counts, zeroes, bytes)) return false;
    remaining -= bytes;
  }

  uint64_t offset = dataStart(state.header);
  const uint64_t fileSize = state.file.fileSize64();
  while (offset < fileSize) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(state.file, offset, fileSize, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.role != RowRole::ShelfReference) {
      if (row.ownerIndex >= state.header.descriptorCount) return false;
      const uint64_t at = static_cast<uint64_t>(row.ownerIndex) * sizeof(uint32_t);
      uint32_t count = 0;
      if (!counts.seekSet64(at) || !readExact(counts, &count, sizeof(count)) || count == UINT32_MAX) return false;
      ++count;
      if (!counts.seekSet64(at) || !writeExact(counts, &count, sizeof(count))) return false;
    }
    offset = next;
  }
  if (offset != fileSize) return false;

  offset = dataStart(state.header);
  uint32_t descriptorIndex = 0;
  while (offset < fileSize) {
    DescriptorRecord disk{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(state.file, offset, fileSize, kind, disk, row, next)) return false;
    if (kind == RECORD_DESCRIPTOR) {
      Descriptor descriptor;
      uint32_t count = 0;
      const uint64_t at = static_cast<uint64_t>(descriptorIndex) * sizeof(count);
      if (descriptorIndex >= state.header.descriptorCount ||
          !readDescriptorAt(state.file, offset, fileSize, descriptor) || !counts.seekSet64(at) ||
          !readExact(counts, &count, sizeof(count)) || count > INT32_MAX)
        return false;
      descriptor.record.complete = 1;
      descriptor.record.total = static_cast<int32_t>(count);
      descriptor.record.page = 1;
      descriptor.record.nextIndex = NO_LINK;
      descriptor.record.prevIndex = NO_LINK;
      if (!updateDescriptor(state.file, descriptor)) return false;
      ++descriptorIndex;
    }
    offset = next;
  }
  if (offset != fileSize || descriptorIndex != state.header.descriptorCount || !counts.close()) return false;
  Storage.remove(BOOK_INDEX_COUNTS_WORK);
  return true;
}
}  // namespace

bool BookOrbitCatalogStore::available() {
  if (!Storage.ready() || !KOREADER_STORE.hasCredentials()) return false;
  return recover();
}

bool BookOrbitCatalogStore::beginRebuild() {
  if (rebuildState.phase == RebuildPhase::Snapshot || rebuildState.phase == RebuildPhase::Finalize) cancelRebuild();
  if (rebuildState.file) rebuildState.file.close();
  rebuildState.bookIds.close();
  closeSourceIdSets(rebuildState);
  removeSourceIdSetFiles();
  Storage.remove(TEMP);
  Storage.remove(BOOK_INDEX_TEMP);
  Storage.remove(BOOK_INDEX_COUNTS_WORK);
  rebuildState.settings = BookOrbitCatalogSettings{};
  rebuildState.header = {};
  rebuildState.cursor = 0;
  rebuildState.query.clear();
  rebuildState.requestQuery.clear();
  rebuildState.cursorToken.clear();
  rebuildState.generation.clear();
  rebuildState.etag.clear();
  rebuildState.snapshotTotal = -1;
  rebuildState.expectedPages = 0;
  rebuildState.pagesFetched = 0;
  rebuildState.booksFetched = 0;
  rebuildState.currentBookId = 0;
  rebuildState.currentBookHasSelectedMembership = false;
  rebuildState.currentBookEntry = nullptr;
  rebuildState.rootIndex = 0;
  rebuildState.allBooksIndex = 0;
  memset(rebuildState.sourceSectionIndices, 0, sizeof(rebuildState.sourceSectionIndices));
  rebuildState.continueCount = 0;
  rebuildState.discoverCount = 0;
  memset(rebuildState.seenContinue, 0, sizeof(rebuildState.seenContinue));
  memset(rebuildState.seenDiscover, 0, sizeof(rebuildState.seenDiscover));
  rebuildState.error = BookOrbitCatalogClient::JSON_ERROR;
  rebuildState.generationStartMs = millis();
  rebuildState.stageStartMs = rebuildState.generationStartMs;
  rebuildState.stage = "setup";
  rebuildState.phase = RebuildPhase::Idle;
  BookOrbitCatalogClient::lastHttpCode = 0;

  if (!Storage.ready() || !KOREADER_STORE.hasCredentials()) {
    failGeneration("storage/credentials", BookOrbitCatalogClient::NO_CREDENTIALS);
    return false;
  }
  if (!rebuildState.settings.load()) {
    LOG_ERR("BOCat", "Could not load catalog settings; keeping the current snapshot");
    failGeneration("settings-load", BookOrbitCatalogClient::JSON_ERROR);
    return false;
  }
  if (!buildSnapshotQuery(rebuildState.settings, rebuildState.query)) {
    failGeneration("snapshot-query/settings", BookOrbitCatalogClient::JSON_ERROR);
    return false;
  }
  LOG_INF("BOCat", "Generation begin scope=%s sources=%u supported_only=%u",
          rebuildState.settings.scope == BookOrbitCatalogSettings::Scope::All ? "all" : "selected",
          static_cast<unsigned>(rebuildState.settings.sources.size()), rebuildState.settings.supportedOnly ? 1U : 0U);
  const std::string base = KOREADER_STORE.getBaseUrl();
  const std::string& username = KOREADER_STORE.getUsername();
  if (base.size() > UINT16_MAX || username.size() > UINT16_MAX) {
    failGeneration("account-metadata", BookOrbitCatalogClient::JSON_ERROR);
    return false;
  }
  if (!Storage.ensureDirectoryExists("/.crosspoint/bookorbit")) {
    failGeneration("cache-directory", BookOrbitCatalogClient::NETWORK_ERROR);
    return false;
  }
  recover();
  if (!rebuildState.bookIds.open() || !openSourceIdSets(rebuildState)) {
    LOG_ERR("BOCat", "Could not create snapshot ID indexes");
    failGeneration("snapshot-id-index-open", BookOrbitCatalogClient::NETWORK_ERROR);
    return false;
  }

  rebuildState.header.magic = FILE_MAGIC;
  rebuildState.header.version = VERSION;
  rebuildState.header.headerBytes = sizeof(Header);
  rebuildState.header.baseLength = static_cast<uint16_t>(base.size());
  rebuildState.header.usernameLength = static_cast<uint16_t>(username.size());
  rebuildState.header.canonicalTotal = -1;
  if (!Storage.openFileForWrite("BOCat", TEMP, rebuildState.file) ||
      !writeExact(rebuildState.file, &rebuildState.header, sizeof(rebuildState.header)) ||
      !writeExact(rebuildState.file, base.data(), base.size()) ||
      !writeExact(rebuildState.file, username.data(), username.size())) {
    failGeneration("staging-file-open/header-write", BookOrbitCatalogClient::NETWORK_ERROR);
    return false;
  }
  if (!appendInitialNavigation(rebuildState)) {
    failGeneration("snapshot-navigation-create", BookOrbitCatalogClient::JSON_ERROR);
    return false;
  }
  rebuildState.requestQuery = rebuildState.query;
  rebuildState.requestQuery.reserve(rebuildState.query.size() + MAX_URL_BYTES * 3 + 8);
  finishGenerationStage("snapshot");
  rebuildState.phase = RebuildPhase::Snapshot;
  return true;
}

void BookOrbitCatalogStore::cancelRebuild() {
  if (rebuildState.phase != RebuildPhase::Snapshot && rebuildState.phase != RebuildPhase::Finalize) return;
  if (rebuildState.file) rebuildState.file.close();
  rebuildState.bookIds.close();
  closeSourceIdSets(rebuildState);
  rebuildState.currentBookEntry = nullptr;
  Storage.remove(TEMP);
  Storage.remove(BOOK_INDEX_TEMP);
  Storage.remove(BOOK_INDEX_COUNTS_WORK);
  rebuildState.phase = RebuildPhase::Cancelled;
  rebuildState.error = BookOrbitCatalogClient::JSON_ERROR;
}

BookOrbitCatalogClient::Error BookOrbitCatalogStore::rebuildError() { return rebuildState.error; }

BookOrbitCatalogStore::RebuildStatus BookOrbitCatalogStore::rebuildStep(RebuildProgress* progress) {
  RebuildStatus result = RebuildStatus::Failed;
  switch (rebuildState.phase) {
    case RebuildPhase::Idle:
    case RebuildPhase::Failed:
      result = RebuildStatus::Failed;
      break;
    case RebuildPhase::Complete:
      result = RebuildStatus::Complete;
      break;
    case RebuildPhase::Cancelled:
      result = RebuildStatus::Cancelled;
      break;
    case RebuildPhase::Snapshot: {
      if (rebuildState.pagesFetched > 0 && rebuildState.cursorToken.empty()) {
        if (rebuildState.booksFetched != static_cast<uint32_t>(rebuildState.snapshotTotal) ||
            rebuildState.bookIds.size() != static_cast<uint32_t>(rebuildState.snapshotTotal) ||
            !appendSnapshotShelves(rebuildState) || !validateSnapshotSources(rebuildState) ||
            !completeSnapshotDescriptors(rebuildState) || !writeHeader(rebuildState.file, rebuildState.header))
          return failGeneration("snapshot-finalize/descriptors", BookOrbitCatalogClient::JSON_ERROR);
        finishGenerationStage("validate");
        rebuildState.phase = RebuildPhase::Finalize;
        result = RebuildStatus::InProgress;
        break;
      }
      if (rebuildState.pagesFetched > 0 && rebuildState.pagesFetched >= rebuildState.expectedPages)
        return failGeneration("snapshot-page-limit", BookOrbitCatalogClient::JSON_ERROR);

      rebuildState.requestQuery = rebuildState.query;
      if (!rebuildState.cursorToken.empty()) {
        static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
        rebuildState.requestQuery += "&cursor=";
        for (const unsigned char byte : rebuildState.cursorToken) {
          const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                  (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
                                  byte == '~';
          if (unreserved) {
            rebuildState.requestQuery.push_back(static_cast<char>(byte));
          } else {
            rebuildState.requestQuery.push_back('%');
            rebuildState.requestQuery.push_back(HEX_DIGITS[byte >> 4]);
            rebuildState.requestQuery.push_back(HEX_DIGITS[byte & 0x0F]);
          }
        }
      }

      rebuildState.booksOnPage = 0;
      BookOrbitSnapshotPage page;
      const BookOrbitSnapshotCallbacks callbacks{&rebuildState, snapshotBook, snapshotSource, snapshotMembership,
                                                 snapshotShelf};
#if LOG_LEVEL >= 3
      const uint32_t pageStartMs = millis();
      const uint32_t rowsBefore = rebuildState.header.rowCount;
#endif
      const BookOrbitCatalogClient::Error error =
          BookOrbitCatalogClient::fetchSnapshot(rebuildState.requestQuery, callbacks, page);
#if LOG_LEVEL >= 3
      const uint32_t ingestionMs = static_cast<uint32_t>(millis()) - pageStartMs;
#endif
      if (error != BookOrbitCatalogClient::OK) return failGeneration("snapshot-fetch", error);
      if (rebuildState.currentBookId > 0 && rebuildState.settings.scope == BookOrbitCatalogSettings::Scope::Selected &&
          !rebuildState.currentBookHasSelectedMembership)
        return failGeneration("snapshot-selected-membership", BookOrbitCatalogClient::JSON_ERROR);
      rebuildState.currentBookId = 0;
      rebuildState.currentBookHasSelectedMembership = false;
      rebuildState.currentBookEntry = nullptr;
      if (page.version != 1 || page.total < 0 || page.generation.empty() || page.etag.empty() ||
          page.generation.size() > MAX_URL_BYTES || page.etag.size() > MAX_URL_BYTES ||
          page.nextCursor.size() > MAX_URL_BYTES)
        return failGeneration("snapshot-page-metadata", BookOrbitCatalogClient::JSON_ERROR);

      if (rebuildState.pagesFetched == 0) {
        rebuildState.snapshotTotal = page.total;
        rebuildState.generation = page.generation;
        rebuildState.etag = page.etag;
        rebuildState.expectedPages =
            page.total == 0
                ? 1U
                : static_cast<uint32_t>((static_cast<uint64_t>(page.total) + BOOK_PAGE_SIZE - 1) / BOOK_PAGE_SIZE);
      } else if (page.total != rebuildState.snapshotTotal || page.generation != rebuildState.generation ||
                 page.etag != rebuildState.etag) {
        return failGeneration("snapshot-freeze-mismatch", BookOrbitCatalogClient::JSON_ERROR);
      }

      if (rebuildState.booksFetched > static_cast<uint32_t>(rebuildState.snapshotTotal))
        return failGeneration("snapshot-total-overflow", BookOrbitCatalogClient::JSON_ERROR);
      const uint32_t remaining = static_cast<uint32_t>(rebuildState.snapshotTotal) - rebuildState.booksFetched;
      const uint32_t expectedBooks = std::min<uint32_t>(BOOK_PAGE_SIZE, remaining);
      const bool morePagesExpected = rebuildState.pagesFetched + 1 < rebuildState.expectedPages;
      if (rebuildState.booksOnPage != expectedBooks || page.nextCursor.empty() == morePagesExpected ||
          (!page.nextCursor.empty() && page.nextCursor == rebuildState.cursorToken))
        return failGeneration("snapshot-page-count/cursor", BookOrbitCatalogClient::JSON_ERROR);

      if (rebuildState.pagesFetched == UINT32_MAX || rebuildState.booksFetched > UINT32_MAX - rebuildState.booksOnPage)
        return failGeneration("snapshot-counter-overflow", BookOrbitCatalogClient::JSON_ERROR);
      ++rebuildState.pagesFetched;
      rebuildState.booksFetched += rebuildState.booksOnPage;
      rebuildState.cursor = rebuildState.pagesFetched;
      rebuildState.cursorToken = page.nextCursor;
      if (!writeHeader(rebuildState.file, rebuildState.header))
        return failGeneration("snapshot-page-write", BookOrbitCatalogClient::NETWORK_ERROR);
#if LOG_LEVEL >= 3
      LOG_TRC("BOCat", "Generation snapshot_page=%lu rows=%lu ingest_ms=%lu",
              static_cast<unsigned long>(rebuildState.pagesFetched),
              static_cast<unsigned long>(rebuildState.header.rowCount - rowsBefore),
              static_cast<unsigned long>(ingestionMs));
#endif
      result = RebuildStatus::InProgress;
      break;
    }
    case RebuildPhase::Finalize: {
      Header& header = rebuildState.header;
      HalFile& file = rebuildState.file;
      if (rebuildState.bookIds.size() > INT32_MAX ||
          rebuildState.bookIds.size() != static_cast<uint32_t>(rebuildState.snapshotTotal))
        return failGeneration("canonical-book-count", BookOrbitCatalogClient::JSON_ERROR);
      header.canonicalTotal = static_cast<int32_t>(rebuildState.bookIds.size());
      if (!writeHeader(file, header))
        return failGeneration("final-header-write", BookOrbitCatalogClient::NETWORK_ERROR);
      file.flush();
      const uint64_t stagedFileSize = file.fileSize64();
      if (!writeBookIndexTemp(rebuildState.bookIds, file, header, rebuildState.sourceFingerprint))
        return failGeneration("book-index-write", BookOrbitCatalogClient::NETWORK_ERROR);
      if (!file.close() || !validateFile(TEMP))
        return failGeneration("staging-file-validate", BookOrbitCatalogClient::JSON_ERROR);
      finishGenerationStage("publish");
      recover();
      invalidateSnapshotFingerprint();
      if (!Storage.publishStagedFileWithBackup(TEMP, LIVE, BACKUP))
        return failGeneration("snapshot-publish", BookOrbitCatalogClient::NETWORK_ERROR);
      if (!validateFile(LIVE)) {
        Storage.remove(LIVE);
        if (Storage.exists(BACKUP)) Storage.rename(BACKUP, LIVE);
        return failGeneration("live-snapshot-validate", BookOrbitCatalogClient::JSON_ERROR);
      }
      cacheSnapshotFingerprint(header, stagedFileSize, rebuildState.sourceFingerprint);
      if (!publishBookIndex()) LOG_ERR("BOCat", "Could not publish derived canonical book index");
      Storage.remove(BACKUP);
      finishGenerationStage("complete");
      LOG_INF("BOCat", "Generation complete total_ms=%lu books=%lu rows=%lu descriptors=%lu",
              static_cast<unsigned long>(static_cast<uint32_t>(millis()) - rebuildState.generationStartMs),
              static_cast<unsigned long>(rebuildState.bookIds.size()), static_cast<unsigned long>(header.rowCount),
              static_cast<unsigned long>(header.descriptorCount));
      rebuildState.bookIds.close();
      closeSourceIdSets(rebuildState);
      rebuildState.error = BookOrbitCatalogClient::OK;
      rebuildState.phase = RebuildPhase::Complete;
      result = RebuildStatus::Complete;
      break;
    }
  }
  fillRebuildProgress(progress);
  return result;
}

BookOrbitCatalogClient::Error BookOrbitCatalogStore::rebuild(void (*progress)(void*, uint32_t), void* context) {
  if (!beginRebuild()) return rebuildError();
  uint32_t reported = 0;
  for (;;) {
    RebuildProgress current;
    const RebuildStatus status = rebuildStep(&current);
    if (progress && current.completedPages != reported) {
      reported = current.completedPages;
      progress(context, reported);
    }
    switch (status) {
      case RebuildStatus::InProgress:
        break;
      case RebuildStatus::Complete:
        return BookOrbitCatalogClient::OK;
      case RebuildStatus::Failed:
        return rebuildError();
      case RebuildStatus::Cancelled:
        return BookOrbitCatalogClient::JSON_ERROR;
    }
  }
}

bool BookOrbitCatalogStore::listing(const std::string& url, const BookOrbitCatalogListParser::EntryCallback& callback,
                                    BookOrbitPageInfo& outPage) {
  if (url.compare(0, sizeof(SEARCH_PREFIX) - 1, SEARCH_PREFIX) == 0) {
    const size_t separator = url.find(':', sizeof(SEARCH_PREFIX) - 1);
    if (separator == std::string::npos) return false;
    const uint32_t page = static_cast<uint32_t>(strtoul(url.c_str() + sizeof(SEARCH_PREFIX) - 1, nullptr, 10));
    return search(url.substr(separator + 1), page, callback, outPage);
  }
  if (!available()) return false;
  HalFile file;
  Header header{};
  if (!Storage.openFileForRead("BOCat", LIVE, file) || !readHeader(file, header, true)) return false;
  bool authors = false;
  if (url.compare(0, strlen(LOCAL_PREFIX), LOCAL_PREFIX) == 0 || isAggregateSection(url, authors))
    return localListing(file, header, url, callback, outPage);

  const std::string key = normalizeUrl(url);
  uint32_t requestedPage = 0;
  if (!canonicalPageNumber(url, requestedPage)) return false;
  const std::string family = pageFamilyUrl(url);
  const uint64_t size = file.fileSize64();
  uint64_t offset = dataStart(header);
  uint32_t index = 0;
  uint32_t selectedIndex = UINT32_MAX;
  Descriptor selectedDescriptor;
  bool exactMatch = false;
  while (offset < size) {
    DescriptorRecord disk{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, size, kind, disk, row, next)) return false;
    if (kind == RECORD_DESCRIPTOR) {
      Descriptor descriptor;
      if (index >= header.descriptorCount || !readDescriptorAt(file, offset, size, descriptor)) return false;
      if (normalizeUrl(descriptor.url) == key) {
        selectedIndex = index;
        selectedDescriptor = std::move(descriptor);
        exactMatch = true;
        break;
      }
      if (selectedIndex == UINT32_MAX && disk.complete && disk.page == 1 && pageFamilyUrl(descriptor.url) == family) {
        selectedIndex = index;
        selectedDescriptor = std::move(descriptor);
      }
      ++index;
    }
    offset = next;
  }
  if (selectedIndex == UINT32_MAX || !selectedDescriptor.record.complete) return false;

  uint32_t expected = 0;
  if (!scanListing(file, header, selectedIndex, {}, &expected)) return false;
  const bool combinedPages = expected > BOOK_PAGE_SIZE;
  if (!exactMatch && !combinedPages) return false;
  bool countOk = false;
  const uint32_t total = familyListingCount(file, header, pageFamilyUrl(selectedDescriptor.url), countOk);
  if (!countOk) return false;

  outPage = {};
  outPage.total = total > INT32_MAX ? INT32_MAX : static_cast<int32_t>(total);
  uint32_t firstRow = 0;
  uint32_t endRow = expected;
  if (combinedPages) {
    const uint32_t lastPage =
        total == 0 ? 1U : static_cast<uint32_t>((static_cast<uint64_t>(total) + BOOK_PAGE_SIZE - 1) / BOOK_PAGE_SIZE);
    if (requestedPage > lastPage) return false;
    outPage.page = static_cast<int32_t>(requestedPage);
    firstRow = (requestedPage - 1) * BOOK_PAGE_SIZE;
    endRow = std::min<uint32_t>(expected, firstRow + BOOK_PAGE_SIZE);
    const auto pageUrl = [&](uint32_t page) {
      std::string result = pageFamilyUrl(selectedDescriptor.url);
      result += result.find('?') == std::string::npos ? "?page=" : "&page=";
      result += std::to_string(page);
      return result;
    };
    if (requestedPage > 1) outPage.prevUrl = pageUrl(requestedPage - 1);
    if (requestedPage < lastPage) outPage.nextUrl = pageUrl(requestedPage + 1);
  } else {
    outPage.page = selectedDescriptor.record.page;
    Descriptor link;
    if (selectedDescriptor.record.nextIndex != NO_LINK) {
      if (!findDescriptor(file, header, selectedDescriptor.record.nextIndex, link)) return false;
      outPage.nextUrl = link.url;
    }
    if (selectedDescriptor.record.prevIndex != NO_LINK) {
      if (!findDescriptor(file, header, selectedDescriptor.record.prevIndex, link)) return false;
      outPage.prevUrl = link.url;
    }
  }

  uint32_t scanned = 0;
  uint32_t emitted = 0;
  bool callbackOk = true;
  const auto showEntry = [&](const BookOrbitEntry& sourceEntry) {
    if (!callback || !callbackOk || scanned < firstRow || scanned >= endRow) {
      ++scanned;
      return;
    }
    std::string countUrl;
    BookOrbitCatalogSettings::SourceKind sourceKind{};
    int32_t sourceId = 0;
    bool hasCountTarget = false;
    if (sourceEntry.type == BookOrbitEntryType::NAVIGATION && sourceForHref(sourceEntry.href, sourceKind, sourceId)) {
      countUrl = sourceBooksUrl(sourceKind, sourceId);
      hasCountTarget = true;
    } else if (sourceEntry.type == BookOrbitEntryType::NAVIGATION &&
               sourceKindForSection(sourceEntry.href, sourceKind)) {
      countUrl = sourceEntry.href;
      hasCountTarget = true;
    }
    if (hasCountTarget) {
      BookOrbitEntry entry = sourceEntry;
      uint32_t filteredCount = 0;
      bool found = false;
      if (!sourceListingCount(file, header, countUrl, filteredCount, found)) {
        callbackOk = false;
        ++scanned;
        return;
      }
      if (found) entry.count = filteredCount > INT32_MAX ? INT32_MAX : static_cast<int32_t>(filteredCount);
      callback(entry);
    } else {
      callback(sourceEntry);
    }
    ++scanned;
    ++emitted;
  };
  BookOrbitCatalogListParser::EntryCallback visibleCallback;
  if (callback) visibleCallback = showEntry;
  uint32_t actual = 0;
  if (!scanListing(file, header, selectedIndex, visibleCallback, &actual)) return false;
  const uint32_t expectedEmitted = endRow > firstRow ? endRow - firstRow : 0;
  return callbackOk && actual == expected && (!callback || emitted == expectedEmitted);
}

bool BookOrbitCatalogStore::dashboard(const BookOrbitCatalogListParser::EntryCallback& callback) {
  if (!available()) return false;
  HalFile file;
  Header header{};
  if (!Storage.openFileForRead("BOCat", LIVE, file) || !readHeader(file, header, true)) return false;
  const uint64_t size = file.fileSize64();
  uint64_t offset = dataStart(header);
  while (offset < size) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, size, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.role == RowRole::ShelfReference && row.ownerIndex == 0 && callback) {
      BookOrbitEntry entry;
      uint64_t canonicalOffset = 0;
      if (!findCanonicalBook(file, header, row.bookId, canonicalOffset) ||
          !decodeRow(file, canonicalOffset, size, entry))
        return false;
      entry.group = static_cast<BookOrbitEntryGroup>(row.group);
      callback(entry);
    }
    offset = next;
  }
  uint8_t emittedNavigation = 0;
  bool callbackOk = true;
  const auto emitRoot = [&](const BookOrbitEntry& sourceEntry) {
    if (!callback || !callbackOk || sourceEntry.type != BookOrbitEntryType::NAVIGATION || emittedNavigation >= 8)
      return;
    BookOrbitEntry entry = sourceEntry;
    std::string countUrl;
    BookOrbitCatalogSettings::SourceKind sourceKind{};
    int32_t sourceId = 0;
    bool hasCountTarget = false;
    if (sourceForHref(sourceEntry.href, sourceKind, sourceId)) {
      countUrl = sourceBooksUrl(sourceKind, sourceId);
      hasCountTarget = true;
    } else if (sourceKindForSection(sourceEntry.href, sourceKind)) {
      countUrl = sourceEntry.href;
      hasCountTarget = true;
    }
    bool aggregateAuthors = false;
    if (isAggregateSection(sourceEntry.href, aggregateAuthors)) {
      BookOrbitPageInfo aggregatePage;
      if (!localListing(file, header, sourceEntry.href, {}, aggregatePage)) {
        callbackOk = false;
        return;
      }
      entry.count = aggregatePage.total;
    } else if (hasCountTarget) {
      uint32_t count = 0;
      bool found = false;
      if (!sourceListingCount(file, header, countUrl, count, found)) {
        callbackOk = false;
        return;
      }
      if (found) entry.count = count > INT32_MAX ? INT32_MAX : static_cast<int32_t>(count);
    }
    ++emittedNavigation;
    callback(entry);
  };
  if (!scanListing(file, header, 0, emitRoot, nullptr)) return false;
  return callbackOk;
}

bool BookOrbitCatalogStore::findBook(int32_t id, BookOrbitEntry& result) {
  if (!available()) return false;
  HalFile file;
  Header header{};
  if (!Storage.openFileForRead("BOCat", LIVE, file) || !readHeader(file, header, true)) return false;
  uint64_t offset = dataStart(header);
  const uint64_t size = file.fileSize64();
  while (offset < size) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, size, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.type == static_cast<uint8_t>(BookOrbitEntryType::BOOK) &&
        row.role == RowRole::CanonicalBook && row.bookId == id)
      return decodeRow(file, offset, size, result);
    offset = next;
  }
  return false;
}

bool BookOrbitCatalogStore::search(const std::string& query, uint32_t page,
                                   const BookOrbitCatalogListParser::EntryCallback& callback,
                                   BookOrbitPageInfo& outPage) {
  if (!available() || page == 0) return false;
  // One reusable 512-byte field buffer; too large for the firmware task stack.
  auto workspace = makeUniqueNoThrow<char[]>(MAX_FIELD_BYTES);
  if (!workspace) {
    LOG_ERR("BOCat", "Offline search workspace allocation failed (%u bytes)", MAX_FIELD_BYTES);
    return false;
  }
  HalFile file;
  Header header{};
  if (!Storage.openFileForRead("BOCat", LIVE, file) || !readHeader(file, header, true)) return false;
  const uint64_t begin = static_cast<uint64_t>(page - 1) * 12;
  const uint64_t end = begin + 12;
  uint32_t matchesCount = 0;
  const uint64_t size = file.fileSize64();
  uint64_t offset = dataStart(header);
  while (offset < size) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, size, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.role == RowRole::CanonicalBook) {
      bool match = false;
      if (!rowMatches(file, offset, row, query, workspace.get(), match)) return false;
      if (match) ++matchesCount;
    }
    offset = next;
  }
  outPage = {};
  outPage.total = matchesCount > INT32_MAX ? INT32_MAX : static_cast<int32_t>(matchesCount);
  outPage.page = page;
  const uint32_t lastPage = (matchesCount + 11) / 12;
  if (page > 1) outPage.prevUrl = std::string(SEARCH_PREFIX) + std::to_string(page - 1) + ":" + query;
  if (page < lastPage) outPage.nextUrl = std::string(SEARCH_PREFIX) + std::to_string(page + 1) + ":" + query;
  uint32_t matched = 0;
  offset = dataStart(header);
  while (offset < size) {
    DescriptorRecord descriptor{};
    RowRecord row{};
    uint8_t kind = 0;
    uint64_t next = 0;
    if (!readRecordHeader(file, offset, size, kind, descriptor, row, next)) return false;
    if (kind == RECORD_ROW && row.role == RowRole::CanonicalBook) {
      bool match = false;
      if (!rowMatches(file, offset, row, query, workspace.get(), match)) return false;
      if (match) {
        if (matched >= begin && matched < end && callback) {
          BookOrbitEntry entry;
          if (!decodeRow(file, offset, size, entry)) return false;
          callback(entry);
        }
        ++matched;
      }
    }
    offset = next;
  }
  return true;
}
