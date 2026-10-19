#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "StagedFileReplace.h"

// Minimal host card model: FAT rename is a same-volume move and refuses an occupied destination.
// Faults are injected after a completed rename to model power loss between directory operations.
class FatStorageAdapter {
 public:
  struct PowerLoss final : std::runtime_error {
    PowerLoss() : std::runtime_error("simulated power loss") {}
  };

  StagedFileReplace::Operations operations() {
    return {this, &existsThunk, &renameThunk};
  }

  void put(const char* path, std::string contents) { files_[path] = std::move(contents); }
  bool contains(const char* path) const { return files_.find(path) != files_.end(); }
  const std::string& read(const char* path) const { return files_.at(path); }
  void remove(const char* path) { files_.erase(path); }

  int failRenameCall = 0;
  int losePowerAfterRenameCall = 0;

 private:
  static bool existsThunk(void* context, const char* path) {
    return static_cast<FatStorageAdapter*>(context)->contains(path);
  }

  static bool renameThunk(void* context, const char* oldPath, const char* newPath) {
    auto& self = *static_cast<FatStorageAdapter*>(context);
    ++self.renameCalls_;
    if (self.renameCalls_ == self.failRenameCall) return false;
    const auto source = self.files_.find(oldPath);
    if (source == self.files_.end() || self.files_.find(newPath) != self.files_.end()) return false;
    self.files_.emplace(newPath, std::move(source->second));
    self.files_.erase(source);
    if (self.renameCalls_ == self.losePowerAfterRenameCall) throw PowerLoss{};
    return true;
  }

  std::map<std::string, std::string> files_;
  int renameCalls_ = 0;
};
