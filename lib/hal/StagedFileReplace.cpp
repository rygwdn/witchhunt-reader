#include "StagedFileReplace.h"

namespace StagedFileReplace {

bool publish(const Operations& operations, const char* tempPath, const char* livePath, const char* backupPath) {
  if (!operations.exists || !operations.rename || !tempPath || !livePath || !backupPath ||
      !operations.exists(operations.context, tempPath) || operations.exists(operations.context, backupPath)) {
    return false;
  }

  const bool hadLive = operations.exists(operations.context, livePath);
  if (hadLive && !operations.rename(operations.context, livePath, backupPath)) return false;
  if (operations.rename(operations.context, tempPath, livePath)) return true;

  if (hadLive) operations.rename(operations.context, backupPath, livePath);
  return false;
}

}  // namespace StagedFileReplace
