#include "ReadingSessionTracker.h"
#include <algorithm>


#include <Arduino.h>  // millis()
#include <HalClock.h>
#include <Logging.h>

#include "BookOrbitReadingStatsOutbox.h"
#include "BookOrbitBookStateStore.h"
#include "BookOrbitBookStateSync.h"
#include "ReadingStats.h"


ReadingSessionTracker& globalReadingSessionTracker() {
  static ReadingSessionTracker instance;
  return instance;
}

uint32_t ReadingSessionTracker::flushIdleSinceLastActivity(bool* idleGapCapped) {
  if (idleGapCapped) *idleGapCapped = false;
  if (!active) return 0;
  const uint32_t now = millis();
  const uint32_t delta = now - lastActivityMs;  // unsigned wrap is fine
  // Cap idle gaps. A user idle for an hour shouldn't get credited for it.
  if (idleGapCapped) *idleGapCapped = delta > MAX_IDLE_MS;
  const uint32_t capped = delta > MAX_IDLE_MS ? MAX_IDLE_MS : delta;
  accumulatedMs += capped;
  lastActivityMs = now;
  return capped;
}

void ReadingSessionTracker::recordBookOrbitEvent(const uint32_t durationMs) {
  if (bookOrbitHash.empty() || durationMs == 0) return;
  if (!HalClock::isSynced() || lastActivityEpoch <= 0) {
    pendingBookEventMs = 0;
    pendingBookEventStartTime = 0;
    return;
  }

  if (pendingBookEventMs == 0) {
    pendingBookEventStartTime = static_cast<uint32_t>(lastActivityEpoch);
    pendingBookEventLocationTick = lastBookLocationTick;
  }
  pendingBookEventMs += durationMs;
  while (pendingBookEventMs >= 1000) {
    const uint32_t durationSeconds =
        static_cast<uint32_t>(std::min<uint64_t>(pendingBookEventMs / 1000, 86400));
    const BookOrbitPageStatEvent event{
        .page = pendingBookEventLocationTick,
        .startTime = pendingBookEventStartTime,
        .durationSeconds = durationSeconds,
        .totalPages = BOOKORBIT_LOCATION_TICKS,
    };
    if (!BookOrbitReadingStatsOutbox::append(bookOrbitHash, event)) {
      LOG_ERR("BOStats", "Could not persist page event for %s", bookOrbitHash.c_str());
      return;
    }
    pendingBookEventMs -= durationSeconds * 1000;
    pendingBookEventStartTime += durationSeconds;
    if (pendingBookEventMs == 0) {
      pendingBookEventStartTime = 0;
    } else {
      // A carried fractional second begins at the page active during this interval.
      pendingBookEventLocationTick = lastBookLocationTick;
    }
  }
}

void ReadingSessionTracker::begin(const std::string& docId_, const std::string& title_, const std::string& author_,
                                  const std::string& bookOrbitHash_, const uint32_t initialLocationTick) {
  if (active) {
    // Don't lose data from a previous session that wasn't explicitly closed.
    end();
  }
  active = true;
  docId = docId_;
  title = title_;
  author = author_;
  bookOrbitHash = bookOrbitHash_;
  walltimeStartEpoch = HalClock::isSynced() ? static_cast<int64_t>(HalClock::now()) : 0;
  lastActivityMs = millis();
  lastActivityEpoch = HalClock::isSynced() ? HalClock::now() : 0;
  accumulatedMs = 0;
  pagesTurnedThisSession = 0;
  lastKnownProgress = 0;
  lastBookLocationTick =
      initialLocationTick <= BOOKORBIT_LOCATION_TICKS ? initialLocationTick : BOOKORBIT_LOCATION_TICKS;
  pendingBookEventMs = 0;
  pendingBookEventStartTime = 0;
  pendingBookEventLocationTick = lastBookLocationTick;
  LOG_DBG("RST", "Session begin doc=%s sync=%d", docId.c_str(), HalClock::isSynced() ? 1 : 0);
}

void ReadingSessionTracker::onPageTurn() {
  if (!active) return;
  flushIdleSinceLastActivity();
  ++pagesTurnedThisSession;
  lastActivityEpoch = HalClock::isSynced() ? HalClock::now() : 0;
}

void ReadingSessionTracker::onBookPageTurn() {
  if (!active) return;
  bool idleGapCapped = false;
  const uint32_t elapsed = flushIdleSinceLastActivity(&idleGapCapped);
  recordBookOrbitEvent(elapsed);
  if (idleGapCapped && pendingBookEventMs < 1000) {
    // The remaining fraction belongs before the idle gap. Do not let its synthetic timestamp
    // carry into activity after the gap; sub-second precision is intentionally dropped here.
    pendingBookEventMs = 0;
    pendingBookEventStartTime = 0;
  }
  ++pagesTurnedThisSession;
  lastActivityEpoch = HalClock::isSynced() ? HalClock::now() : 0;
}

void ReadingSessionTracker::onBookPageTurn(const uint32_t locationTick) {
  if (!active) return;
  bool idleGapCapped = false;
  const uint32_t elapsed = flushIdleSinceLastActivity(&idleGapCapped);
  recordBookOrbitEvent(elapsed);
  if (idleGapCapped && pendingBookEventMs < 1000) {
    // See the no-location overload: an old fractional second cannot bridge a capped idle gap.
    pendingBookEventMs = 0;
    pendingBookEventStartTime = 0;
  }
  ++pagesTurnedThisSession;
  lastBookLocationTick = locationTick <= BOOKORBIT_LOCATION_TICKS ? locationTick : BOOKORBIT_LOCATION_TICKS;
  lastActivityEpoch = HalClock::isSynced() ? HalClock::now() : 0;
}

void ReadingSessionTracker::updateProgress(const uint8_t progress) {
  if (!active) return;
  lastKnownProgress = progress > 100 ? 100 : progress;
}

void ReadingSessionTracker::markFinished() {
  if (!active) return;
  const int64_t walltime = HalClock::isSynced() ? static_cast<int64_t>(HalClock::now()) : 0;
  const auto result = READING_STATS.markFinished(docId, title, author, static_cast<time_t>(walltime));
  if (result != ReadingStatsStore::WriteResult::Done) {
    LOG_ERR("RST", "markFinished not recorded (%u) doc=%s wall=%lld", static_cast<unsigned>(result), docId.c_str(),
            (long long)walltime);
  }
  LOG_DBG("RST", "Marked finished doc=%s wall=%lld", docId.c_str(), (long long)walltime);
  if (!bookOrbitHash.empty()) {
    std::string modifiedDay;
    if (BookOrbitBookStateSync::currentModifiedDay(modifiedDay)) {
      if (BookOrbitBookStateStore::queueFinished(bookOrbitHash, modifiedDay) ==
          BookOrbitBookStateStore::QueueResult::IoError) {
        LOG_ERR("RST", "Could not queue BookOrbit finished status for %s", bookOrbitHash.c_str());
      }
    }
  }
}

void ReadingSessionTracker::end() {
  if (!active) return;
  // Final idle flush so we credit the time between the last page turn and now,
  // capped at MAX_IDLE_MS just like all the other gaps.
  const uint32_t elapsed = flushIdleSinceLastActivity();
  recordBookOrbitEvent(elapsed);

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
    // One book and the global figures, written in place copy-on-write: ~10 KB of heap whatever the history holds.
    const auto result = READING_STATS.recordSession(docId, title, author, seconds, pagesTurnedThisSession,
                                                    lastKnownProgress, static_cast<time_t>(walltime));
    if (result != ReadingStatsStore::WriteResult::Done) {
      LOG_ERR("RST", "Session not recorded (%u) doc=%s secs=%u pages=%u wall=%lld", static_cast<unsigned>(result),
              docId.c_str(), seconds, pagesTurnedThisSession, (long long)walltime);
    }
  }

  active = false;
  docId.clear();
  title.clear();
  bookOrbitHash.clear();
  author.clear();
  lastActivityMs = 0;
  lastActivityEpoch = 0;
  accumulatedMs = 0;
  pagesTurnedThisSession = 0;
  lastBookLocationTick = 0;
  lastKnownProgress = 0;
  pendingBookEventMs = 0;
  pendingBookEventStartTime = 0;
  pendingBookEventLocationTick = 0;
}


uint32_t ReadingSessionTracker::getLiveSeconds() const {
  if (!active) return 0;
  // Best-effort live readout — does not mutate state, so it does not include
  // the in-flight idle gap. Good enough for "you've been reading for X".
  return static_cast<uint32_t>(accumulatedMs / 1000);
}
