#pragma once
#include <cstdio>

#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#define LOG_ERR(origin, format, ...) \
  std::fprintf(stderr, "[ERR][%s] " format "\n", origin __VA_OPT__(,) __VA_ARGS__)
#define LOG_DBG(origin, format, ...) \
  std::fprintf(stderr, "[DBG][%s] " format "\n", origin __VA_OPT__(,) __VA_ARGS__)
#define LOG_INF(origin, format, ...) \
  std::fprintf(stderr, "[INF][%s] " format "\n", origin __VA_OPT__(,) __VA_ARGS__)
#if LOG_LEVEL >= 3
#define LOG_TRC(origin, format, ...) \
  std::fprintf(stderr, "[TRC][%s] " format "\n", origin __VA_OPT__(,) __VA_ARGS__)
#else
#define LOG_TRC(origin, format, ...)
#endif
