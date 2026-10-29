#pragma once
#include <string_view>

namespace OpdsSyncPolicy {
// ISO UTC timestamps include exactly three fractional digits in the local outbox.
// A completion confirmation wins if the last page was saved in the same millisecond.
inline bool replacesProgress(std::string_view previous, float previousProgress, std::string_view modified,
                             float progress) {
  return previous.empty() || modified > previous || (modified == previous && progress > previousProgress);
}
// An in-flight upload may only acknowledge the snapshot it actually sent.
inline bool matchesQueuedProgress(std::string_view sentModified, float sentProgress, std::string_view queuedModified,
                                  float queuedProgress) {
  return sentModified == queuedModified && sentProgress == queuedProgress;
}
inline bool automaticEnabled(bool hasKoCredentials, bool associated, bool preference) {
  return preference && (associated || hasKoCredentials);
}
// Different renderers round a fractional position to the start of a local page.
inline bool sameReadingPage(float pageStart, float pageEnd, float remote, float epsilon) {
  return remote >= pageStart - epsilon && remote <= pageEnd + epsilon;
}
}  // namespace OpdsSyncPolicy
