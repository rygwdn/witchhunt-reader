#include "DiskLogger.h"

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

#include "CrossPointSettings.h"

std::atomic<uint32_t> DiskLogger::linesSinceFlush{0};
uint32_t DiskLogger::flushedSequence = 0;
std::atomic<bool> DiskLogger::reentrant{false};
// Guards against cross-task concurrent flushes (render task vs. main task).
// STATIC allocation, deliberately, not xSemaphoreCreateMutex(): the same reasoning
// Logging.cpp documents for its own mutex — the flush path is reachable while the
// heap is in states where creating a mutex would touch heap metadata we have twice
// recorded being corrupted by early allocations. A StaticSemaphore_t lives in .bss.
// Non-blocking trylock below: whichever task loses the race skips this flush cycle;
// the ring buffer content persists in RTC memory and will be written next time.
static StaticSemaphore_t flushMutexStorage;
static SemaphoreHandle_t flushMutex = nullptr;

std::string DiskLogger::getLogFilePath(int generation) {
  if (generation <= 0) {
    return std::string(LOG_BASE_PATH) + LOG_EXTENSION;
  }
  return std::string(LOG_BASE_PATH) + "." + std::to_string(generation) + LOG_EXTENSION;
}

void DiskLogger::begin() {
  if (flushMutex == nullptr) {
    flushMutex = xSemaphoreCreateMutexStatic(&flushMutexStorage);
  }
  if (SETTINGS.diskLogs != 0) {
    writeRingBufferToFile();
    rotateGenerations();
  }
  setDiskLogCallback(&DiskLogger::logLine);
}

void DiskLogger::logLine(const char* /*line*/) {
  if (SETTINGS.diskLogs == 0 || reentrant.load()) return;
  if ((linesSinceFlush.fetch_add(1) + 1) % FLUSH_INTERVAL == 0) {
    writeRingBufferToFile();
  }
}

void DiskLogger::flushNow() {
  // Mirrors logLine()'s gate: callers (settings-save hook) fire on every
  // settings write regardless of whether disk logging is enabled, so the
  // no-op-when-off behavior has to live here rather than at each call site.
  if (SETTINGS.diskLogs == 0 || reentrant.load()) return;
  writeRingBufferToFile();
}

void DiskLogger::clear() {
  for (int gen = 0; gen < MAX_LOG_GENERATIONS; gen++) {
    Storage.remove(getLogFilePath(gen).c_str());
  }
}

void DiskLogger::rotateGenerations() {
  // Drop the oldest generation, then shift each remaining one up by one slot, oldest
  // shift first so we never clobber a file we haven't moved yet.
  Storage.remove(getLogFilePath(MAX_LOG_GENERATIONS - 1).c_str());
  for (int gen = MAX_LOG_GENERATIONS - 2; gen >= 0; --gen) {
    Storage.rename(getLogFilePath(gen).c_str(), getLogFilePath(gen + 1).c_str());
  }
}

void DiskLogger::writeRingBufferToFile() {
  // Non-blocking: if another task is already flushing, skip this cycle.
  // The ring buffer lives in RTC memory and will be written on the next flush.
  if (flushMutex == nullptr || xSemaphoreTake(flushMutex, 0) != pdTRUE) return;

  // Set reentrant before any SD access so that LOG_ERR calls from HalStorage
  // (which would re-enter logLine -> here) are caught and short-circuited above.
  reentrant = true;

  uint32_t nextSequence = flushedSequence;
  std::string content = getLogLinesSince(nextSequence);
  const std::string activePath = getLogFilePath(0);
  if (!content.empty()) {
    // Rotate BEFORE appending: the size check reads the current file, and rotation
    // must complete while the path is still the pre-append one.
    size_t activeSize = 0;
    {
      HalFile check = Storage.open(activePath.c_str());
      if (check) activeSize = check.fileSize();
    }
    if (activeSize > MAX_LOG_FILE_SIZE) rotateGenerations();
    // Open with O_WRITE | O_CREAT (no truncate), seek to end for append semantics.
    HalFile file = Storage.open(activePath.c_str(), O_WRITE | O_CREAT);
    if (file) {
      file.seek(file.fileSize());
      const size_t written = file.write(content.c_str(), content.size());
      file.flush();
      if (written == content.size()) flushedSequence = nextSequence;
    }
  }

  reentrant = false;
  xSemaphoreGive(flushMutex);
}
