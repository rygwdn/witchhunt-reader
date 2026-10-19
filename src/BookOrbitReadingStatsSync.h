#pragma once

namespace BookOrbitReadingStatsSync {

// Sends queued events through the BookOrbit plugin API and removes only batches
// whose complete, per-book idempotent acknowledgement was returned.
void drain();

}  // namespace BookOrbitReadingStatsSync
