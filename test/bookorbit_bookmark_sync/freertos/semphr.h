#pragma once

#include "FreeRTOS.h"

struct BookmarkWorkerTestSemaphore {};
using SemaphoreHandle_t = BookmarkWorkerTestSemaphore*;

SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
