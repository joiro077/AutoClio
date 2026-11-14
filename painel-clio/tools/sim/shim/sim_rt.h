// sim_rt.h - imitacao minima do ESP-IDF/FreeRTOS para rodar o painel no PC (so para tirar prints)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// sdkconfig
#define CONFIG_PAINEL_ELM_UART_TX 17
#define CONFIG_PAINEL_ELM_UART_RX 11
#define CONFIG_PAINEL_ELM_BAUD 38400
#define CONFIG_PAINEL_IGN_SENSE_GPIO (-1)
#define CONFIG_PAINEL_POWER_HOLD_GPIO (-1)

// esp_err / esp_log / esp_timer
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
int64_t esp_timer_get_time(void);   // microssegundos de tempo SIMULADO

// uart / gpio
#define UART_NUM_1 1

// FreeRTOS
typedef uint32_t TickType_t;        // 1 tick = 1 ms simulado
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   int prio, TaskHandle_t *h, int core);
void vTaskDelay(TickType_t t);
void vTaskDelayUntil(TickType_t *prev, TickType_t inc);
TickType_t xTaskGetTickCount(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t to);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m);
QueueHandle_t xQueueCreate(int len, int item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t to);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t to);