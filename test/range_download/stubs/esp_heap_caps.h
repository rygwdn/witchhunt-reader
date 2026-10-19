#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_DEFAULT = 0;
inline size_t downloaderLargestBlock = 16372;
inline size_t heap_caps_get_largest_free_block(int) { return downloaderLargestBlock; }
inline unsigned esp_get_free_heap_size() { return 100000; }
