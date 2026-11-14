// elm327.h - conversa com o ELM327 por UART (fio)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "elm_parse.h"

esp_err_t elm_init(int uart_num, int tx_gpio, int rx_gpio, int baud);

// Envia um comando (sem o \r) e le ate o prompt '>'. resp recebe o texto bruto.
elm_status_t elm_cmd(const char *cmd, char *resp, size_t max, uint32_t timeout_ms);

// Comando AT que deve responder "OK" (ou qualquer coisa sem erro).
bool elm_at(const char *cmd, uint32_t timeout_ms);

// Envia bytes hex (ex.: "21A9") e devolve os bytes da resposta.
elm_status_t elm_request(const char *hex, uint8_t *out, size_t max, size_t *n, uint32_t timeout_ms);
