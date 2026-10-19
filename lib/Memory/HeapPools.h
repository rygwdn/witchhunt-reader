#pragma once

#include <esp_heap_caps.h>
#include <esp_system.h>
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
#include <esp_memory_utils.h>
#endif

#include <cstddef>
#include <cstdint>
#include <memory>

// Pool-aware heap measurement and explicit placement for PSRAM boards.
//
// The Arduino S3 variants ship CONFIG_SPIRAM_USE_MALLOC=1 with a 4096-byte preference
// threshold. Firmware wraps heap_caps_malloc_extmem_enable() so ESP-IDF's early PSRAM init
// and Arduino's psramAddToHeap() cannot enable automatic PSRAM fallback for ordinary malloc/new.
// The S3 linker also wraps application/library calls to raw heap_caps allocation APIs, adding
// MALLOC_CAP_INTERNAL unless the request explicitly includes MALLOC_CAP_SPIRAM or
// MALLOC_CAP_SPIRAM_NO_ENC; MALLOC_CAP_8BIT/DEFAULT can otherwise match PSRAM. Intentional
// external buffers use explicit PSRAM capabilities, including psramAllocBytes(). See audit §9.
//
// Heap gates that protect ordinary allocations measure internal RAM on PSRAM boards. On the C3
// there is one allocator and one pool, so the #else branches below preserve the expressions the
// floors were tuned against.
//
// Logging may still report both pools. SystemStatus reports them separately (internal free/min/max
// plus PSRAM free/min/max/total), because a mixed row hides the S3's real state.

// Note on the Arduino wrappers: ESP.getFreeHeap(), ESP.getMinFreeHeap() and
// ESP.getMaxAllocHeap() query MALLOC_CAP_INTERNAL in this framework
// (framework-arduinoespressif32/cores/esp32/Esp.cpp:159-174). Keep new heap decisions routed
// through these helpers so the pool being protected remains explicit.

// Free bytes in the pool a gate can actually run out of.
inline uint32_t internalFreeBytes() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#else
  return static_cast<uint32_t>(esp_get_free_heap_size());
#endif
}

// Largest contiguous allocatable block in that same pool.
inline uint32_t internalContigBytes() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#else
  return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
#endif
}

// Low-water mark of that pool since boot.
inline uint32_t internalMinEverFreeBytes() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#else
  return static_cast<uint32_t>(esp_get_minimum_free_heap_size());
#endif
}

// Would freeing `block` hand bytes back to the internal pool a caller under pressure is short of?
//
// Some display backends explicitly place framebuffers or auxiliary planes in PSRAM, while others
// use ordinary malloc and now land in internal RAM. A pointer check avoids degrading display
// quality when freeing an explicitly external buffer cannot help an internal-RAM allocation.
// A null pointer answers "yes", preserving the conservative pre-PSRAM behavior when placement is
// unknown.
inline bool freeingReclaimsInternal(const void* block) {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  return block == nullptr || !esp_ptr_external_ram(block);
#else
  (void)block;
  return true;
#endif
}

// Explicitly external allocation, for buffers deliberately placed in PSRAM instead of consuming
// scarce internal RAM: large, cold, rebuildable, and never DMA or ISR reachable. MALLOC_CAP_SPIRAM
// has no internal fallback, so a null return means PSRAM cannot satisfy this request; the caller
// must degrade to its internal/pre-PSRAM path rather than silently spending internal RAM.
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM

struct PsramFree {
  void operator()(uint8_t* p) const { heap_caps_free(p); }
};
using PsramBytes = std::unique_ptr<uint8_t[], PsramFree>;

inline PsramBytes psramAllocBytes(const size_t bytes) {
  PsramBytes block(static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM)));
  return block;
}
#endif
