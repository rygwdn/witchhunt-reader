#pragma once

#include <cstdint>

#include <Arduino.h>

inline uint32_t internalFreeBytes() { return ESP.getFreeHeap(); }
inline uint32_t internalContigBytes() { return ESP.getMaxAllocHeap(); }
