#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// Flushes only log lines not yet persisted from the RTC ring to the SD card.
// Enabled via SETTINGS.diskLogs; the sink is registered once after storage and
// settings load. The ring remains the crash-report source when disk logging is off.
class DiskLogger {
 public:
  static void begin();
  // Callback registered with setDiskLogCallback(). Flushes after a ring cycle.
  static void logLine(const char* line);
  // Flush new ring entries immediately (e.g. after enabling the setting).
  static void flushNow();
  // Delete all log file generations from SD card.
  static void clear();

  // Path for a given generation: 0 = the currently active log, 1..getGenerationCount()-1
  // = rotated-out backups, oldest last (debug.log.txt, debug.log.1.txt, debug.log.2.txt, ...).
  // Used by the log viewer to enumerate files; doesn't check the file actually exists.
  static std::string getLogFilePath(int generation);
  static constexpr int getGenerationCount() { return MAX_LOG_GENERATIONS; }

 private:
  // Root, not /.crosspoint: a plain .txt name at the top level is one a reporter
  // can find in any file browser without knowing where the app's private folder
  // lives, and is grabbable over USB-MSC/webdav without hunting a dotfile.
  static constexpr const char* LOG_BASE_PATH = "/debug.log";
  static constexpr const char* LOG_EXTENSION = ".txt";
  static constexpr size_t MAX_LOG_FILE_SIZE = 512UL * 1024UL;
  // debug.log.txt (active) + this many rotated backups. Rotating per boot rather than
  // keeping one growing file bounds the worst-case SD wear and keeps every generation
  // small enough to page through in the log viewer without excessive seeking.
  static constexpr int MAX_LOG_GENERATIONS = 5;
  // The RTC ring holds 16 lines, so flushing within each cycle bounds the
  // retained interval without doing SD I/O for every log line.
  static constexpr int FLUSH_INTERVAL = 16;

  static std::atomic<uint32_t> linesSinceFlush;
  static uint32_t flushedSequence;
  // HalStorage may log while a flush holds the SD path; those lines remain
  // in the RTC ring for the next flush rather than recursively writing.
  static std::atomic<bool> reentrant;

  static void writeRingBufferToFile();
  // Shifts debug.log.txt -> .1 -> .2 ... dropping whatever falls off the end of
  // MAX_LOG_GENERATIONS. Safe to call when some/all generations don't exist yet:
  // Storage.remove/rename return false without logging when the source is missing.
  static void rotateGenerations();
};
