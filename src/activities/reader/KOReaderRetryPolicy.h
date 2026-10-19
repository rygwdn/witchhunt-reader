#pragma once

#include <cstdint>

#include "../../../lib/KOReaderSync/KOReaderSyncClient.h"

namespace KOReaderRetryPolicy {

enum class AttemptOutcome : uint8_t { COMPLETED, DEFERRED, CANCELLED };
enum class ResultClass : uint8_t { NO_CHANGE, SUCCESS, TRANSIENT_FAILURE, SUSPEND };

struct State {
  uint64_t retryDeadlineUs;
  uint64_t lastObservedUs;
  uint8_t failureCount;
  uint8_t suspended;
};

static constexpr uint64_t MINUTE_US = 60ULL * 1000ULL * 1000ULL;
static constexpr uint8_t MAX_FAILURE_COUNT = 6;

inline uint64_t retryDelayUs(const uint8_t failureCount) {
  if (failureCount == 0) {
    return 0;
  }
  if (failureCount >= MAX_FAILURE_COUNT) {
    return 30ULL * MINUTE_US;
  }
  return (1ULL << (failureCount - 1)) * MINUTE_US;
}

inline ResultClass classify(const KOReaderSyncClient::Error result, const bool isPull, const int httpStatus,
                            const int transportError, const AttemptOutcome outcome) {
  if (outcome == AttemptOutcome::DEFERRED || outcome == AttemptOutcome::CANCELLED) {
    return ResultClass::NO_CHANGE;
  }

  if (result == KOReaderSyncClient::NO_CREDENTIALS) {
    return ResultClass::SUSPEND;
  }
  if (httpStatus == 408 || httpStatus == 429 || (httpStatus >= 500 && httpStatus <= 599)) {
    return ResultClass::TRANSIENT_FAILURE;
  }
  if (result == KOReaderSyncClient::NOT_FOUND) {
    return isPull ? ResultClass::SUCCESS : ResultClass::SUSPEND;
  }
  if (result == KOReaderSyncClient::AUTH_FAILED) {
    return ResultClass::SUSPEND;
  }

  // Explicit transport, parser, and redirect failures are transient even when a proxy supplied
  // a misleading HTTP status alongside them.
  switch (result) {
    case KOReaderSyncClient::NETWORK_ERROR:
    case KOReaderSyncClient::JSON_ERROR:
    case KOReaderSyncClient::REDIRECT_ERROR:
    case KOReaderSyncClient::INVALID_RESPONSE:
      return ResultClass::TRANSIENT_FAILURE;
    case KOReaderSyncClient::OK:
    case KOReaderSyncClient::NO_CREDENTIALS:
    case KOReaderSyncClient::AUTH_FAILED:
    case KOReaderSyncClient::SERVER_ERROR:
    case KOReaderSyncClient::NOT_FOUND:
    case KOReaderSyncClient::USER_EXISTS:
    case KOReaderSyncClient::REGISTRATION_DISABLED:
      break;
  }

  if (httpStatus >= 300 && httpStatus <= 399) {
    return ResultClass::TRANSIENT_FAILURE;
  }
  if (httpStatus >= 400 && httpStatus <= 499) {
    return ResultClass::SUSPEND;
  }
  if (transportError != 0) {
    return ResultClass::TRANSIENT_FAILURE;
  }

  if (result == KOReaderSyncClient::OK) {
    return ResultClass::SUCCESS;
  }
  if (result == KOReaderSyncClient::SERVER_ERROR) {
    return ResultClass::TRANSIENT_FAILURE;
  }
  return ResultClass::SUSPEND;
}

inline void reset(State& state, const uint64_t nowUs) {
  state.retryDeadlineUs = 0;
  state.lastObservedUs = nowUs;
  state.failureCount = 0;
  state.suspended = 0;
}

inline void observeClock(State& state, const uint64_t nowUs) {
  // If RTC power was interrupted, retain the remaining cooldown instead of treating a lower
  // timer reading as an expired deadline. On boards whose LP timer ran through sleep, this path
  // is never needed and the deadline naturally includes the whole sleep interval.
  if (nowUs < state.lastObservedUs && state.retryDeadlineUs > state.lastObservedUs) {
    const uint64_t remainingUs = state.retryDeadlineUs - state.lastObservedUs;
    state.retryDeadlineUs = nowUs > UINT64_MAX - remainingUs ? UINT64_MAX : nowUs + remainingUs;
  }
  state.lastObservedUs = nowUs;
}

inline void record(State& state, const ResultClass result, const uint64_t completedAtUs) {
  switch (result) {
    case ResultClass::NO_CHANGE:
      return;
    case ResultClass::SUCCESS:
      reset(state, completedAtUs);
      return;
    case ResultClass::SUSPEND:
      state.suspended = 1;
      state.retryDeadlineUs = 0;
      state.lastObservedUs = completedAtUs;
      return;
    case ResultClass::TRANSIENT_FAILURE:
      if (state.failureCount < MAX_FAILURE_COUNT) {
        ++state.failureCount;
      }
      const uint64_t delayUs = retryDelayUs(state.failureCount);
      state.retryDeadlineUs = completedAtUs > UINT64_MAX - delayUs ? UINT64_MAX : completedAtUs + delayUs;
      state.lastObservedUs = completedAtUs;
      return;
  }
}

inline bool attemptAllowed(const State& state, const uint64_t nowUs) {
  return state.suspended == 0 && (state.retryDeadlineUs == 0 || nowUs >= state.retryDeadlineUs);
}

}  // namespace KOReaderRetryPolicy
