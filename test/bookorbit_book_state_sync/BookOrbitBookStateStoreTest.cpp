#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <HalStorage.h>
#include <freertos/semphr.h>

#include "BookOrbitBookStateStore.h"

namespace {
constexpr char kHash[] = "0123456789abcdef0123456789abcdef";
constexpr char kModifiedDay[] = "2026-10-03";

std::string testRoot() {
  return (std::filesystem::temp_directory_path() /
          ("bookorbit-book-state-store-test-" +
           std::string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
      .string();
}

std::string pendingPath() {
  return (std::filesystem::path(testRoot()) / ".crosspoint/bookorbit-book-state/pending" /
          (std::string(kHash) + ".state"))
      .string();
}
}  // namespace

SemaphoreHandle_t xSemaphoreCreateMutex() {
  static BookmarkWorkerTestSemaphore semaphore;
  return &semaphore;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*) {
  static BookmarkWorkerTestSemaphore semaphore;
  return &semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdPASS; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdPASS; }

class BookOrbitBookStateStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::filesystem::remove_all(testRoot());
    HalStorage::getInstance().setCrosspointRoot(testRoot());
  }

  void TearDown() override {
    HalStorage::getInstance().setCrosspointRoot("");
    std::filesystem::remove_all(testRoot());
  }
};

TEST_F(BookOrbitBookStateStoreTest, SelectsAndRecoversBackupOnlyPendingRecord) {
  ASSERT_EQ(BookOrbitBookStateStore::queueRating(kHash, 4, kModifiedDay),
            BookOrbitBookStateStore::QueueResult::Queued);
  const std::string livePath = pendingPath();
  ASSERT_TRUE(std::filesystem::exists(livePath));

  std::error_code error;
  std::filesystem::rename(livePath, livePath + ".bak", error);
  ASSERT_FALSE(error);

  BookOrbitBookStateStore::State pending;
  ASSERT_TRUE(BookOrbitBookStateStore::nextPending("", true, pending));
  EXPECT_EQ(pending.hash, kHash);
  EXPECT_TRUE(pending.pendingRating);
  EXPECT_EQ(pending.rating, 4);
  EXPECT_TRUE(std::filesystem::exists(livePath));
  EXPECT_FALSE(std::filesystem::exists(livePath + ".bak"));
}

TEST_F(BookOrbitBookStateStoreTest, AcknowledgingOlderStatusRevisionKeepsNewerEditPending) {
  using Status = BookOrbitCatalogClient::ReadStatus;
  ASSERT_EQ(BookOrbitBookStateStore::queueStatus(kHash, Status::Read, kModifiedDay),
            BookOrbitBookStateStore::QueueResult::Queued);
  BookOrbitBookStateStore::State sent;
  ASSERT_TRUE(BookOrbitBookStateStore::read(kHash, sent));
  ASSERT_TRUE(sent.pendingStatus);

  ASSERT_EQ(BookOrbitBookStateStore::queueStatus(kHash, Status::Abandoned, kModifiedDay),
            BookOrbitBookStateStore::QueueResult::Queued);
  ASSERT_TRUE(BookOrbitBookStateStore::acknowledge(sent, true, false));

  BookOrbitBookStateStore::State current;
  ASSERT_TRUE(BookOrbitBookStateStore::read(kHash, current));
  EXPECT_TRUE(current.pendingStatus);
  EXPECT_GT(current.statusRevision, sent.statusRevision);
  EXPECT_EQ(current.localStatus, Status::Abandoned);
}

TEST_F(BookOrbitBookStateStoreTest, ExactAcknowledgmentRetainsCanonicalServerValueAndTimestamp) {
  ASSERT_EQ(BookOrbitBookStateStore::queueRating(kHash, 4, kModifiedDay),
            BookOrbitBookStateStore::QueueResult::Queued);
  BookOrbitBookStateStore::State sent;
  ASSERT_TRUE(BookOrbitBookStateStore::read(kHash, sent));

  constexpr char kServerUpdatedAt[] = "2026-10-03T12:00:00Z";
  ASSERT_TRUE(BookOrbitBookStateStore::rememberCanonicalRating(kHash, true, 3, kServerUpdatedAt));
  ASSERT_TRUE(BookOrbitBookStateStore::acknowledge(sent, false, true));

  BookOrbitBookStateStore::State current;
  ASSERT_TRUE(BookOrbitBookStateStore::read(kHash, current));
  EXPECT_FALSE(current.pendingRating);
  EXPECT_TRUE(current.canonicalRatingSet);
  EXPECT_EQ(current.canonicalRating, 3);
  EXPECT_EQ(current.ratingUpdatedAt, kServerUpdatedAt);
}
