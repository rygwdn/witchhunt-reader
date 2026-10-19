#pragma once

#include "FreeRTOS.h"

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char* name, uint32_t stackBytes, void* argument,
                                   UBaseType_t priority, TaskHandle_t* handle, BaseType_t core);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t timeout);
void vTaskDelay(TickType_t delay);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
void vTaskDelete(TaskHandle_t task);
