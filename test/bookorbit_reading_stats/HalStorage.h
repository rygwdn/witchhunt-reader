#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <StagedFileReplace.h>

#include <Print.h>
class HalFile {
 public:
  bool seek(size_t offset) {
    if (!bytes_ || offset > bytes_->size()) return false;
    position_ = offset;
    return true;
  }
  bool seekSet64(uint64_t offset) {
    if (!bytes_ || offset > bytes_->size()) return false;
    position_ = static_cast<size_t>(offset);
    return true;
  }
  uint64_t size64() const { return bytes_ ? bytes_->size() : 0; }
  size_t size() const { return bytes_ ? bytes_->size() : 0; }
  uint64_t position64() const { return position_; }
  size_t read(void* out, size_t length) {
    if (!bytes_) return 0;
    const size_t count = std::min(length, bytes_->size() - position_);
    if (count) std::memcpy(out, bytes_->data() + position_, count);
    position_ += count;
    return count;
  }
  size_t write(const void* input, size_t length) {
    if (!bytes_ || !writable_) return 0;
    if (position_ > bytes_->size()) bytes_->resize(position_);
    if (position_ + length > bytes_->size()) bytes_->resize(position_ + length);
    if (length) std::memcpy(bytes_->data() + position_, input, length);
    position_ += length;
    return length;
  }
  size_t write(const uint8_t* input, size_t length) { return write(static_cast<const void*>(input), length); }
  void flush() {}

 private:
  friend class HalStorage;
  void bind(std::vector<uint8_t>* bytes, const bool writable) {
    bytes_ = bytes;
    writable_ = writable;
    position_ = 0;
  }

  std::vector<uint8_t>* bytes_ = nullptr;
  size_t position_ = 0;
  bool writable_ = false;
};

using FsFile = HalFile;

class HalStorage {
 public:
  struct PowerLoss final : std::runtime_error {
    PowerLoss() : std::runtime_error("simulated power loss") {}
  };
  bool ensureDirectoryExists(const char*) { return true; }
  bool exists(const char* path) const { return files_.find(path) != files_.end(); }
  bool remove(const char* path) { return files_.erase(path) != 0; }
  bool rename(const char* oldPath, const char* newPath) {
    if (exists(newPath)) return false;  // FAT rename never replaces an occupied destination.
    auto node = files_.extract(oldPath);
    if (node.empty()) return false;
    node.key() = newPath;
    files_.insert(std::move(node));
    if (++renameCalls_ == powerLossAfterRenameCall_) throw PowerLoss{};
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    const auto found = files_.find(path);
    if (found == files_.end()) return false;
    file.bind(&found->second, false);
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    auto& bytes = files_[path];
    bytes.clear();
    file.bind(&bytes, true);
    return true;
  }
  bool openFileForUpdate(const char*, const char* path, HalFile& file) {
    const auto found = files_.find(path);
    if (found == files_.end()) return false;
    file.bind(&found->second, true);
    return true;
  }
  bool publishStagedFileWithBackup(const char* tempPath, const char* livePath, const char* backupPath) {
    const StagedFileReplace::Operations operations{this, &existsThunk, &renameThunk};
    return StagedFileReplace::publish(operations, tempPath, livePath, backupPath);
  }

  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  void clear() {
    files_.clear();
    setPowerLossAfterRenameCall(0);
  }
  void setPowerLossAfterRenameCall(const int call) {
    renameCalls_ = 0;
    powerLossAfterRenameCall_ = call;
  }
  void put(const char* path, std::vector<uint8_t> bytes) { files_[path] = std::move(bytes); }
  std::vector<uint8_t>& file(const char* path) { return files_.at(path); }

 private:
  static bool existsThunk(void* context, const char* path) {
    return static_cast<HalStorage*>(context)->exists(path);
  }
  static bool renameThunk(void* context, const char* oldPath, const char* newPath) {
    return static_cast<HalStorage*>(context)->rename(oldPath, newPath);
  }

  std::map<std::string, std::vector<uint8_t>> files_;
  int renameCalls_ = 0;
  int powerLossAfterRenameCall_ = 0;
};

#define Storage HalStorage::getInstance()
