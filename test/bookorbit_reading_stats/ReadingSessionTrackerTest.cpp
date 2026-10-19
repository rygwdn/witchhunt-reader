#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <Arduino.h>
#include <HalClock.h>

#include <BookOrbitCatalogClient.h>

#include "BookOrbitBookStateStore.h"
#include "BookOrbitBookStateSync.h"
#include "BookOrbitReadingStatsOutbox.h"
#include "ReadingSessionTracker.h"
#include "ReadingStats.h"

namespace {
constexpr uint32_t kStartEpoch = 1700000000;
const std::string kHash(32, 'b');

void setMillis(const unsigned long value) {
  host_clock::valueRef() = value;
  host_clock::stepRef() = 0;
}
}  // namespace

ReadingStatsStore ReadingStatsStore::instance;

ReadingStatsStore::WriteResult ReadingStatsStore::recordSession(const std::string&, const std::string&,
                                                                const std::string&, const uint32_t,
                                                                const uint32_t, const uint8_t, const time_t) {
  return WriteResult::Done;
}

ReadingStatsStore::WriteResult ReadingStatsStore::markFinished(const std::string&, const std::string&,
                                                               const std::string&, const time_t) {
  return WriteResult::Done;
}

bool BookOrbitBookStateSync::currentModifiedDay(std::string&) { return false; }

BookOrbitBookStateStore::QueueResult BookOrbitBookStateStore::queueFinished(const std::string&,
                                                                            const std::string&) {
  return QueueResult::Skipped;
}

TEST(ReadingSessionTracker, CappedIdleGapDoesNotCarryOldFractionIntoLaterActivity) {
  Storage.clear();
  setMillis(0);
  HalClock::set(kStartEpoch);

  ReadingSessionTracker tracker;
  tracker.begin("doc", "title", "author", kHash, 10);

  setMillis(1500);
  HalClock::set(kStartEpoch + 2);
  tracker.onBookPageTurn(20);

  setMillis(100000);  // More than MAX_IDLE_MS; the credited interval is capped at 90 seconds.
  HalClock::set(kStartEpoch + 100);
  tracker.onBookPageTurn(30);

  setMillis(100500);
  HalClock::set(kStartEpoch + 101);
  tracker.onBookPageTurn(40);

  setMillis(101100);
  HalClock::set(kStartEpoch + 102);
  tracker.onBookPageTurn(50);

  BookOrbitPageStatsBook batch;
  ASSERT_TRUE(BookOrbitReadingStatsOutbox::nextBatch(batch, "", true));
  ASSERT_EQ(batch.events.size(), 3u);
  EXPECT_EQ(batch.events[0].durationSeconds, 1u);
  EXPECT_EQ(batch.events[0].startTime, kStartEpoch);
  EXPECT_EQ(batch.events[1].durationSeconds, 90u);
  EXPECT_EQ(batch.events[1].startTime, kStartEpoch + 1);
  EXPECT_EQ(batch.events[2].durationSeconds, 1u);
  EXPECT_EQ(batch.events[2].startTime, kStartEpoch + 100);
}
