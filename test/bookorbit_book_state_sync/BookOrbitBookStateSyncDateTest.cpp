#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>

#include <HalClock.h>

#include "BookOrbitBookStateSync.h"
#include "CrossPointSettings.h"

BookmarkWorkerTestSettings SETTINGS;

namespace {
bool clockSynced = true;
time_t clockNow = 0;
}  // namespace

namespace HalClock {
bool isSynced() { return clockSynced; }
time_t now() { return clockNow; }
}  // namespace HalClock

#if defined(_WIN32) && !defined(__CYGWIN__)
TEST(BookOrbitBookStateSyncDate, UsesUtcDayWhenConfiguredDeviceZoneDiffers) { GTEST_SKIP(); }
#else
TEST(BookOrbitBookStateSyncDate, UsesUtcDayWhenConfiguredDeviceZoneDiffers) {
  using namespace std::chrono;
  const auto instant = sys_days{year{2026} / June / 6} + hours{6} + minutes{30};
  clockNow = system_clock::to_time_t(instant);
  clockSynced = true;
  ASSERT_EQ(setenv("TZ", "PST8PDT", 1), 0);
  tzset();

  std::tm local{};
  ASSERT_NE(localtime_r(&clockNow, &local), nullptr);
  ASSERT_EQ(local.tm_mday, 5);

  std::string modifiedDay;
  ASSERT_TRUE(BookOrbitBookStateSync::currentModifiedDay(modifiedDay));
  EXPECT_EQ(modifiedDay, "2026-06-06");

  unsetenv("TZ");
  tzset();
}
#endif

TEST(BookOrbitBookStateSyncDate, ClearsDayWhenClockIsNotSynced) {
  clockSynced = false;
  std::string modifiedDay = "stale";
  EXPECT_FALSE(BookOrbitBookStateSync::currentModifiedDay(modifiedDay));
  EXPECT_TRUE(modifiedDay.empty());
  clockSynced = true;
}
