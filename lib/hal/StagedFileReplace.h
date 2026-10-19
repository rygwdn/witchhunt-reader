#pragma once

namespace StagedFileReplace {

// Path operations supplied by HalStorage in firmware and a FAT-semantics adapter in host tests.
// The caller serializes these operations with its store mutex; this helper never holds the HAL
// storage mutex while another operation/callback runs.
struct Operations {
  void* context;
  bool (*exists)(void* context, const char* path);
  bool (*rename)(void* context, const char* oldPath, const char* newPath);
};

// Publish a fully written, flushed, closed, and caller-validated temp file over livePath without
// relying on FAT rename-overwrite. backupPath must be absent (the caller recovers it first).
// On success, backupPath is intentionally retained until the caller validates livePath and removes
// the backup. On synchronous publish failure, restore the old live file when possible.
bool publish(const Operations& operations, const char* tempPath, const char* livePath, const char* backupPath);

}  // namespace StagedFileReplace
