#include "ReadingSessionTracker.h"

#include <Arduino.h>  // millis()
#include <HalClock.h>
#include <Logging.h>
#include <esp_random.h>

#include "ReadingStats.h"
#include "network/OpdsStatsSync.h"

ReadingSessionTracker& globalReadingSessionTracker() {
  static ReadingSessionTracker instance;
  return instance;
}

void ReadingSessionTracker::flushIdleSinceLastActivity() {
  if (!active) return;
  const uint32_t now = millis();
  const uint32_t delta = now - lastActivityMs;  // unsigned wrap is fine
  // Cap idle gaps. A user idle for an hour shouldn't get credited for it.
  const uint32_t capped = delta > MAX_IDLE_MS ? MAX_IDLE_MS : delta;
  accumulatedMs += capped;
  lastActivityMs = now;
}

void ReadingSessionTracker::begin(const std::string& docId_, const std::string& title_, const std::string& author_,
                                  const std::string& bookPath_) {
  if (active) {
    // Don't lose data from a previous session that wasn't explicitly closed.
    end();
  }
  active = true;
  docId = docId_;
  title = title_;
  author = author_;
  bookPath = bookPath_;
  uint8_t random[16];
  for (unsigned i = 0; i < 16; i += 4) {
    const uint32_t value = esp_random();
    for (unsigned j = 0; j < 4; ++j) random[i + j] = static_cast<uint8_t>(value >> (j * 8));
  }
  random[6] = (random[6] & 0x0f) | 0x40;
  random[8] = (random[8] & 0x3f) | 0x80;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  unsigned cursor = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) sessionId[cursor++] = '-';
    sessionId[cursor++] = HEX_DIGITS[random[i] >> 4];
    sessionId[cursor++] = HEX_DIGITS[random[i] & 15];
  }
  sessionId[cursor] = 0;
  sessionStartMs = millis();
  firstKnownProgress = -1;
  walltimeStartEpoch = HalClock::isSynced() ? static_cast<int64_t>(HalClock::now()) : 0;
  lastActivityMs = millis();
  accumulatedMs = 0;
  pagesTurnedThisSession = 0;
  lastKnownProgress = 0;
  LOG_DBG("RST", "Session begin doc=%s sync=%d", docId.c_str(), HalClock::isSynced() ? 1 : 0);
}

void ReadingSessionTracker::onPageTurn() {
  if (!active) return;
  flushIdleSinceLastActivity();
  pagesTurnedThisSession += 1;
}

void ReadingSessionTracker::updateProgress(uint8_t progress) {
  if (!active) return;
  if (firstKnownProgress < 0) firstKnownProgress = progress;
  lastKnownProgress = progress;
}

void ReadingSessionTracker::markFinished() {
  if (!active) return;
  const int64_t walltime = HalClock::isSynced() ? static_cast<int64_t>(HalClock::now()) : 0;
  // Book exit is the only moment the history has to be in RAM; pull it in, merge, write, drop.
  const ReadingStatsStore::ScopedLoad statsLoad;
  READING_STATS.markFinished(docId, title, author, static_cast<time_t>(walltime));
  if (!READING_STATS.saveToFile()) {
    LOG_ERR("RST", "saveToFile failed (markFinished) doc=%s title=%s author=%s wall=%lld", docId.c_str(), title.c_str(),
            author.c_str(), (long long)walltime);
  }
  LOG_DBG("RST", "Marked finished doc=%s wall=%lld", docId.c_str(), (long long)walltime);
}

void ReadingSessionTracker::end() {
  if (!active) return;
  // Final idle flush so we credit the time between the last page turn and now,
  // capped at MAX_IDLE_MS just like all the other gaps.
  flushIdleSinceLastActivity();

  const uint32_t seconds = static_cast<uint32_t>(accumulatedMs / 1000);
  // Prefer the walltime captured at begin(); if HalClock has only just become
  // synced, fall back to "now" as the lastReadEpoch.
  int64_t walltime = walltimeStartEpoch;
  if (walltime == 0 && HalClock::isSynced()) {
    walltime = static_cast<int64_t>(HalClock::now());
  }

  LOG_DBG("RST", "Session end doc=%s secs=%u pages=%u prog=%u wall=%lld", docId.c_str(), seconds,
          pagesTurnedThisSession, lastKnownProgress, (long long)walltime);

  if (!docId.empty()) {
    // See markFinished(): the store is loaded only for this merge-and-save, then released.
    const ReadingStatsStore::ScopedLoad statsLoad;
    READING_STATS.recordSession(docId, title, author, seconds, pagesTurnedThisSession, lastKnownProgress,
                                static_cast<time_t>(walltime));
    if (!READING_STATS.saveToFile()) {
      LOG_ERR("RST", "saveToFile failed (session end) doc=%s title=%s author=%s secs=%u pages=%u wall=%lld",
              docId.c_str(), title.c_str(), author.c_str(), seconds, pagesTurnedThisSession, (long long)walltime);
    }
  }

  if (!bookPath.empty() && seconds && HalClock::isSynced()) {
    const int64_t ended = static_cast<int64_t>(HalClock::now());
    const int64_t started = walltimeStartEpoch > 0
                                ? walltimeStartEpoch
                                : ended - (static_cast<uint64_t>(millis() - sessionStartMs) + 999) / 1000;
    OpdsStatsSync::queue(bookPath, {sessionId, started, ended, seconds, pagesTurnedThisSession, firstKnownProgress,
                                    firstKnownProgress >= 0 ? lastKnownProgress : -1});
  }
  active = false;
  bookPath.clear();
  sessionId[0] = 0;
  docId.clear();
  title.clear();
  author.clear();
  walltimeStartEpoch = 0;
  lastActivityMs = 0;
  accumulatedMs = 0;
  pagesTurnedThisSession = 0;
  lastKnownProgress = 0;
}

uint32_t ReadingSessionTracker::getLiveSeconds() const {
  if (!active) return 0;
  // Best-effort live readout — does not mutate state, so it does not include
  // the in-flight idle gap. Good enough for "you've been reading for X".
  return static_cast<uint32_t>(accumulatedMs / 1000);
}
