#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BookOrbitReadingStatsOutbox.h"
#include "HalStorage.h"

namespace {
constexpr char kJournalPath[] = "/.crosspoint/bookorbit-reading-stats.bin";
constexpr char kBackupPath[] = "/.crosspoint/bookorbit-reading-stats.bak";
constexpr char kTempPath[] = "/.crosspoint/bookorbit-reading-stats.tmp";
constexpr size_t kHeaderBytes = 4;
constexpr size_t kRecordBytes = 52;
const std::string kHash(32, 'a');

BookOrbitPageStatEvent event(const uint32_t page, const uint32_t startTime) {
  return {.page = page, .startTime = startTime, .durationSeconds = 30, .totalPages = 1000000};
}

bool append(const BookOrbitPageStatEvent& item) {
  return BookOrbitReadingStatsOutbox::append(kHash, item);
}

std::vector<uint8_t> prefix(const std::vector<uint8_t>& journal, const size_t recordCount) {
  return std::vector<uint8_t>(journal.begin(), journal.begin() + kHeaderBytes + recordCount * kRecordBytes);
}

TEST(BookOrbitReadingStatsOutbox, RetiresAcknowledgedBatchWithFatRenameRules) {
  Storage.clear();
  ASSERT_TRUE(append(event(10, 1000)));

  BookOrbitPageStatsBook batch;
  ASSERT_TRUE(BookOrbitReadingStatsOutbox::nextBatch(batch, "", true));
  ASSERT_EQ(batch.events.size(), 1u);
  ASSERT_TRUE(BookOrbitReadingStatsOutbox::acknowledge(batch.hash, batch.events));

  BookOrbitPageStatsBook empty;
  EXPECT_FALSE(BookOrbitReadingStatsOutbox::nextBatch(empty, "", true));
  EXPECT_EQ(Storage.file(kJournalPath).size(), kHeaderBytes);
}
TEST(BookOrbitReadingStatsOutbox, RecoversPublishedAckAfterPowerLossBetweenFatRenames) {
  Storage.clear();
  ASSERT_TRUE(append(event(10, 1000)));

  BookOrbitPageStatsBook batch;
  ASSERT_TRUE(BookOrbitReadingStatsOutbox::nextBatch(batch, "", true));
  Storage.setPowerLossAfterRenameCall(1);
  EXPECT_THROW(BookOrbitReadingStatsOutbox::acknowledge(batch.hash, batch.events), HalStorage::PowerLoss);

  Storage.setPowerLossAfterRenameCall(0);
  BookOrbitPageStatsBook empty;
  EXPECT_FALSE(BookOrbitReadingStatsOutbox::nextBatch(empty, "", true));
  EXPECT_EQ(Storage.file(kJournalPath).size(), kHeaderBytes);
  EXPECT_FALSE(Storage.exists(kBackupPath));
  EXPECT_FALSE(Storage.exists(kTempPath));
}

TEST(BookOrbitReadingStatsOutbox, RecoversPublishedAckAfterPowerLossAtFatPublishRename) {
  Storage.clear();
  ASSERT_TRUE(append(event(10, 1000)));

  BookOrbitPageStatsBook batch;
  ASSERT_TRUE(BookOrbitReadingStatsOutbox::nextBatch(batch, "", true));
  Storage.setPowerLossAfterRenameCall(2);
  EXPECT_THROW(BookOrbitReadingStatsOutbox::acknowledge(batch.hash, batch.events), HalStorage::PowerLoss);

  Storage.setPowerLossAfterRenameCall(0);
  BookOrbitPageStatsBook empty;
  EXPECT_FALSE(BookOrbitReadingStatsOutbox::nextBatch(empty, "", true));
  EXPECT_EQ(Storage.file(kJournalPath).size(), kHeaderBytes);
  EXPECT_FALSE(Storage.exists(kBackupPath));
  EXPECT_FALSE(Storage.exists(kTempPath));
}

TEST(BookOrbitReadingStatsOutbox, TailRepairKeepsCompleteLiveAppendsAheadOfOlderBackup) {
  Storage.clear();
  ASSERT_TRUE(append(event(10, 1000)));
  ASSERT_TRUE(append(event(20, 2000)));
  auto& live = Storage.file(kJournalPath);
  Storage.put(kBackupPath, prefix(live, 1));  // stale pre-append copy
  live.push_back(0xA5);                       // torn record tail after both complete appends

  BookOrbitPageStatsBook batch;
  ASSERT_TRUE(BookOrbitReadingStatsOutbox::nextBatch(batch, "", true));

  ASSERT_EQ(batch.events.size(), 2u);
  EXPECT_EQ(batch.events[0].startTime, 1000u);
  EXPECT_EQ(batch.events[1].startTime, 2000u);
  EXPECT_FALSE(Storage.exists(kBackupPath));
  EXPECT_EQ(Storage.file(kJournalPath).size(), kHeaderBytes + 2 * kRecordBytes);
}

TEST(BookOrbitReadingStatsOutbox, TailRepairPreservesValidPrefixBeforeBadFinalCrc) {
  Storage.clear();
  ASSERT_TRUE(append(event(10, 1000)));
  ASSERT_TRUE(append(event(20, 2000)));
  auto& live = Storage.file(kJournalPath);
  Storage.put(kBackupPath, prefix(live, 1));
  live[kHeaderBytes + kRecordBytes + kRecordBytes - 1] ^= 0x80;

  BookOrbitPageStatsBook batch;
  ASSERT_TRUE(BookOrbitReadingStatsOutbox::nextBatch(batch, "", true));

  ASSERT_EQ(batch.events.size(), 1u);
  EXPECT_EQ(batch.events[0].startTime, 1000u);
  EXPECT_FALSE(Storage.exists(kBackupPath));
  EXPECT_EQ(Storage.file(kJournalPath).size(), kHeaderBytes + kRecordBytes);
}

TEST(BookOrbitReadingStatsOutbox, InteriorCorruptionIsRetainedRatherThanFallingBackToOlderBackup) {
  Storage.clear();
  ASSERT_TRUE(append(event(10, 1000)));
  ASSERT_TRUE(append(event(20, 2000)));
  ASSERT_TRUE(append(event(30, 3000)));
  auto& live = Storage.file(kJournalPath);
  const std::vector<uint8_t> damagedLive = live;
  Storage.put(kBackupPath, prefix(live, 1));
  live[kHeaderBytes + kRecordBytes + kRecordBytes - 1] ^= 0x80;  // corrupt middle record
  const std::vector<uint8_t> corrupt = live;

  BookOrbitPageStatsBook batch;
  EXPECT_FALSE(BookOrbitReadingStatsOutbox::nextBatch(batch, "", true));

  EXPECT_EQ(Storage.file(kJournalPath), corrupt);
  EXPECT_EQ(Storage.file(kBackupPath), prefix(damagedLive, 1));
}
}  // namespace
