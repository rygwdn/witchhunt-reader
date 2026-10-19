#include <gtest/gtest.h>

#include "FatStorageAdapter.h"

namespace {
constexpr char kTemp[] = "/journal.tmp";
constexpr char kLive[] = "/journal.bin";
constexpr char kBackup[] = "/journal.bak";

TEST(StagedFileReplace, PublishesOnFatWithoutOverwritingAndKeepsBackupUntilValidated) {
  FatStorageAdapter card;
  card.put(kLive, "old journal");
  card.put(kTemp, "new journal");

  ASSERT_TRUE(StagedFileReplace::publish(card.operations(), kTemp, kLive, kBackup));

  EXPECT_EQ(card.read(kLive), "new journal");
  EXPECT_EQ(card.read(kBackup), "old journal");
  EXPECT_FALSE(card.contains(kTemp));
  card.remove(kBackup);  // caller removes only after validating the new live file
  EXPECT_EQ(card.read(kLive), "new journal");
}

TEST(StagedFileReplace, RollsBackWhenPublishingStagedFileFails) {
  FatStorageAdapter card;
  card.put(kLive, "old journal");
  card.put(kTemp, "new journal");
  card.failRenameCall = 2;  // live->backup succeeds, temp->live fails, rollback is the third move

  EXPECT_FALSE(StagedFileReplace::publish(card.operations(), kTemp, kLive, kBackup));

  EXPECT_EQ(card.read(kLive), "old journal");
  EXPECT_EQ(card.read(kTemp), "new journal");
  EXPECT_FALSE(card.contains(kBackup));
}

TEST(StagedFileReplace, PreservesBothCandidatesIfPowerFailsAfterMovingLiveToBackup) {
  FatStorageAdapter card;
  card.put(kLive, "old journal");
  card.put(kTemp, "new journal");
  card.losePowerAfterRenameCall = 1;

  EXPECT_THROW(StagedFileReplace::publish(card.operations(), kTemp, kLive, kBackup), FatStorageAdapter::PowerLoss);

  EXPECT_FALSE(card.contains(kLive));
  EXPECT_EQ(card.read(kBackup), "old journal");
  EXPECT_EQ(card.read(kTemp), "new journal");
  ASSERT_TRUE(card.operations().rename(card.operations().context, kBackup, kLive));
  EXPECT_EQ(card.read(kLive), "old journal");  // caller recovery can restore old without overwrite
}

TEST(StagedFileReplace, KeepsNewLiveAndOldBackupIfPowerFailsAfterPublishRename) {
  FatStorageAdapter card;
  card.put(kLive, "old journal");
  card.put(kTemp, "new journal");
  card.losePowerAfterRenameCall = 2;

  EXPECT_THROW(StagedFileReplace::publish(card.operations(), kTemp, kLive, kBackup), FatStorageAdapter::PowerLoss);

  EXPECT_EQ(card.read(kLive), "new journal");
  EXPECT_EQ(card.read(kBackup), "old journal");
  EXPECT_FALSE(card.contains(kTemp));
}

TEST(StagedFileReplace, DoesNotClobberAnUnresolvedBackup) {
  FatStorageAdapter card;
  card.put(kLive, "live");
  card.put(kTemp, "staged");
  card.put(kBackup, "recoverable backup");

  EXPECT_FALSE(StagedFileReplace::publish(card.operations(), kTemp, kLive, kBackup));

  EXPECT_EQ(card.read(kLive), "live");
  EXPECT_EQ(card.read(kTemp), "staged");
  EXPECT_EQ(card.read(kBackup), "recoverable backup");
}

TEST(StagedFileReplace, CreatesInitialLiveFileWithoutBackup) {
  FatStorageAdapter card;
  card.put(kTemp, "first journal");

  ASSERT_TRUE(StagedFileReplace::publish(card.operations(), kTemp, kLive, kBackup));

  EXPECT_EQ(card.read(kLive), "first journal");
  EXPECT_FALSE(card.contains(kBackup));
  EXPECT_FALSE(card.contains(kTemp));
}
}  // namespace
