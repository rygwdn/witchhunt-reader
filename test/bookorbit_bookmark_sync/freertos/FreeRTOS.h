#pragma once

#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;
using UBaseType_t = uint32_t;
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);
struct StaticSemaphore_t {};

constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)

// Ownership tests execute synchronously; no competing task can enter these critical sections.
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
