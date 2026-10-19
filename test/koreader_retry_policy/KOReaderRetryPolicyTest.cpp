#include <gtest/gtest.h>

#include "KOReaderRetryPolicy.h"

namespace {
using KOReaderRetryPolicy::AttemptOutcome;
using KOReaderRetryPolicy::ResultClass;
using KOReaderRetryPolicy::State;

TEST(KOReaderRetryPolicyTest, UsesCappedExponentialDeadlinesFromFailureCompletion) {
  constexpr uint64_t minute = KOReaderRetryPolicy::MINUTE_US;
  constexpr uint64_t delays[] = {minute, 2 * minute, 4 * minute, 8 * minute, 16 * minute, 30 * minute, 30 * minute};
  State state{};
  uint64_t completedAtUs = 1234;

  EXPECT_TRUE(KOReaderRetryPolicy::attemptAllowed(state, completedAtUs));
  for (const uint64_t delay : delays) {
    KOReaderRetryPolicy::record(state, ResultClass::TRANSIENT_FAILURE, completedAtUs);
    EXPECT_EQ(state.retryDeadlineUs, completedAtUs + delay);
    EXPECT_FALSE(KOReaderRetryPolicy::attemptAllowed(state, state.retryDeadlineUs - 1));
    EXPECT_TRUE(KOReaderRetryPolicy::attemptAllowed(state, state.retryDeadlineUs));
    completedAtUs = state.retryDeadlineUs;
  }
  EXPECT_EQ(state.failureCount, KOReaderRetryPolicy::MAX_FAILURE_COUNT);
}

TEST(KOReaderRetryPolicyTest, ClassifiesTransientAndSuspendingResults) {
  using Error = KOReaderSyncClient::Error;
  const auto classify = [](const Error result, const bool pull, const int status = 0, const int transportError = 0) {
    return KOReaderRetryPolicy::classify(result, pull, status, transportError, AttemptOutcome::COMPLETED);
  };

  EXPECT_EQ(classify(KOReaderSyncClient::NETWORK_ERROR, false), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::SERVER_ERROR, false), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::JSON_ERROR, false, 400), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::REDIRECT_ERROR, false, 302), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::INVALID_RESPONSE, false, 404), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::SERVER_ERROR, false, 408), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::SERVER_ERROR, false, 429), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::SERVER_ERROR, false, 503), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::NETWORK_ERROR, false, 0, -1), ResultClass::TRANSIENT_FAILURE);
  EXPECT_EQ(classify(KOReaderSyncClient::OK, false, 200), ResultClass::SUCCESS);

  EXPECT_EQ(classify(KOReaderSyncClient::AUTH_FAILED, false, 401), ResultClass::SUSPEND);
  EXPECT_EQ(classify(KOReaderSyncClient::NO_CREDENTIALS, false), ResultClass::SUSPEND);
  EXPECT_EQ(classify(KOReaderSyncClient::SERVER_ERROR, false, 403), ResultClass::SUSPEND);
  EXPECT_EQ(classify(KOReaderSyncClient::SERVER_ERROR, false, 400), ResultClass::SUSPEND);
  EXPECT_EQ(classify(KOReaderSyncClient::USER_EXISTS, false), ResultClass::SUSPEND);
  EXPECT_EQ(classify(KOReaderSyncClient::NOT_FOUND, false, 404), ResultClass::SUSPEND);
}

TEST(KOReaderRetryPolicyTest, PullNotFoundAndSuccessClearPolicy) {
  State state{};
  KOReaderRetryPolicy::record(state, ResultClass::TRANSIENT_FAILURE, 1);
  KOReaderRetryPolicy::record(
      state, KOReaderRetryPolicy::classify(KOReaderSyncClient::NOT_FOUND, true, 404, 0, AttemptOutcome::COMPLETED), 2);
  EXPECT_TRUE(KOReaderRetryPolicy::attemptAllowed(state, 2));
  EXPECT_EQ(state.failureCount, 0);
  EXPECT_EQ(state.retryDeadlineUs, 0);

  KOReaderRetryPolicy::record(state, ResultClass::SUSPEND, 3);
  EXPECT_FALSE(KOReaderRetryPolicy::attemptAllowed(state, 3));
  KOReaderRetryPolicy::record(state, ResultClass::SUCCESS, 4);
  EXPECT_TRUE(KOReaderRetryPolicy::attemptAllowed(state, 4));
  EXPECT_EQ(state.failureCount, 0);
}

TEST(KOReaderRetryPolicyTest, DeferredAndCancelledAttemptsLeaveBackoffUnchanged) {
  State state{};
  KOReaderRetryPolicy::record(state, ResultClass::TRANSIENT_FAILURE, 10);
  const State before = state;
  KOReaderRetryPolicy::record(
      state, KOReaderRetryPolicy::classify(KOReaderSyncClient::NETWORK_ERROR, false, 0, -1, AttemptOutcome::DEFERRED),
      20);
  EXPECT_EQ(state.retryDeadlineUs, before.retryDeadlineUs);
  EXPECT_EQ(state.lastObservedUs, before.lastObservedUs);
  EXPECT_EQ(state.failureCount, before.failureCount);
  EXPECT_EQ(state.suspended, before.suspended);

  KOReaderRetryPolicy::record(
      state, KOReaderRetryPolicy::classify(KOReaderSyncClient::OK, false, 200, 0, AttemptOutcome::CANCELLED), 30);
  EXPECT_EQ(state.retryDeadlineUs, before.retryDeadlineUs);
  EXPECT_EQ(state.lastObservedUs, before.lastObservedUs);
  EXPECT_EQ(state.failureCount, before.failureCount);
  EXPECT_EQ(state.suspended, before.suspended);
}

TEST(KOReaderRetryPolicyTest, TimerRegressionPreservesRemainingCooldown) {
  State state{};
  KOReaderRetryPolicy::reset(state, 1000);
  KOReaderRetryPolicy::record(state, ResultClass::TRANSIENT_FAILURE, 2000);
  KOReaderRetryPolicy::observeClock(state, 500);
  EXPECT_EQ(state.retryDeadlineUs, 500 + KOReaderRetryPolicy::MINUTE_US);
  EXPECT_FALSE(KOReaderRetryPolicy::attemptAllowed(state, state.retryDeadlineUs - 1));
  EXPECT_TRUE(KOReaderRetryPolicy::attemptAllowed(state, state.retryDeadlineUs));
}
}  // namespace
