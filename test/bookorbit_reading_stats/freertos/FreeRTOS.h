#pragma once

#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;
constexpr BaseType_t pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = 0xFFFFFFFFu;
struct StaticSemaphore_t {};
