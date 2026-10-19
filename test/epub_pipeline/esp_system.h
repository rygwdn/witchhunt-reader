#pragma once
// Host stub for <esp_system.h>. Derives from the single ESP stub knob so heap
// pressure simulated with ESP.setFreeHeap() moves every gate together (see the
// esp_heap_caps.h stub next to this one). Pulls in the Arduino shim because
// device code that includes esp headers gets millis() implicitly from the
// Arduino core.
#include <Arduino.h>

#include <cstdint>

inline uint32_t esp_get_free_heap_size() { return ESP.getFreeHeap(); }
inline uint32_t esp_get_minimum_free_heap_size() { return ESP.getMinFreeHeap(); }
