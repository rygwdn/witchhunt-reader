#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

class HalFile {
 public:
  bool seekSet64(uint64_t pos) {
    if (!bytes_) return false;
    if (pos > bytes_->size()) {
      if (!writable_) return false;
      bytes_->resize(static_cast<size_t>(pos));
    }
    position_ = static_cast<size_t>(pos);
    return true;
  }
  uint64_t fileSize64() const { return bytes_ ? bytes_->size() : 0; }
  size_t write(const void* data, size_t len) {
    if (!bytes_ || !writable_) return 0;
    if (position_ + len > bytes_->size()) bytes_->resize(position_ + len);
    if (len) std::memcpy(bytes_->data() + position_, data, len);
    position_ += len;
    return len;
  }
  int read(void* out, size_t len) {
    if (!bytes_ || position_ > bytes_->size()) return 0;
    const size_t n = std::min(len, bytes_->size() - position_);
    if (n) std::memcpy(out, bytes_->data() + position_, n);
    position_ += n;
    return static_cast<int>(n);
  }
  void flush() {}
  bool close() {
    assert(initialized_);
    bytes_ = nullptr;
    return true;
  }
  bool isOpen() const { return bytes_ != nullptr; }
  explicit operator bool() const { return isOpen(); }

 private:
  friend class HalStorage;
  void bind(std::vector<uint8_t>* bytes, bool writable) {
    bytes_ = bytes;
    writable_ = writable;
    position_ = 0;
    initialized_ = true;
  }
  std::vector<uint8_t>* bytes_ = nullptr;
  size_t position_ = 0;
  bool writable_ = false;
  bool initialized_ = false;
};

class HalStorage {
 public:
  bool ready() const { return true; }
  bool ensureDirectoryExists(const char*) { return true; }
  bool exists(const char* path) const { return files_.count(path) != 0; }
  bool remove(const char* path) { return files_.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (exists(to) || !exists(from)) return false;
    files_[to] = std::move(files_[from]);
    files_.erase(from);
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    file.bind(&it->second, false);
    return true;
  }
  bool openFileForUpdate(const char*, const char* path, HalFile& file) {
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    file.bind(&it->second, true);
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    if (failedWritePath_ == path) return false;
    auto& bytes = files_[path];
    bytes.clear();
    file.bind(&bytes, true);
    return true;
  }
  bool publishStagedFileWithBackup(const char* temp, const char* live, const char* backup) {
    if (exists(backup)) return false;
    if (exists(live) && !rename(live, backup)) return false;
    if (!rename(temp, live)) {
      if (exists(backup)) rename(backup, live);
      return false;
    }
    return true;
  }
  static HalStorage& getInstance() {
    static HalStorage value;
    return value;
  }
  void clear() {
    files_.clear();
    failedWritePath_.clear();
  }
  void failWritesTo(const char* path) { failedWritePath_ = path; }
  void put(const char* path, std::vector<uint8_t> bytes) { files_[path] = std::move(bytes); }

 private:
  std::map<std::string, std::vector<uint8_t>> files_;
  std::string failedWritePath_;
};
#define Storage HalStorage::getInstance()
