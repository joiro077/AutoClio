#include "elm327.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "elm";
static int s_uart = -1;

esp_err_t elm_init(int uart_num, int tx, int rx, int baud)
{
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(uart_num, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(rx);   // linha parada em nivel alto se o ELM estiver desligado
    s_uart = uart_num;
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d %d baud", uart_num, tx, rx, baud);
    return ESP_OK;
}

elm_status_t elm_cmd(const char *cmd, char *resp, size_t max, uint32_t timeout_ms)
{
    if (s_uart < 0 || max < 2) return ELM_TIMEOUT;
    uart_flush_input(s_uart);
    uart_write_bytes(s_uart, cmd, strlen(cmd));
    uart_write_bytes(s_uart, "\r", 1);

    size_t len = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    resp[0] = 0;
    while (esp_timer_get_time() < deadline) {
        uint8_t c;
        int r = uart_read_bytes(s_uart, &c, 1, pdMS_TO_TICKS(10));
        if (r <= 0) continue;
        if (c == '>') { resp[len] = 0; return ELM_OK; }
        if (c == 0) continue;                 // alguns clones mandam \0
        if (len < max - 1) resp[len++] = (char)c;
    }
    resp[len] = 0;
    ESP_LOGD(TAG, "timeout em '%s' (recebido: '%s')", cmd, resp);
    return ELM_TIMEOUT;
}

bool elm_at(const char *cmd, uint32_t timeout_ms)
{
    char buf[96];
    if (elm_cmd(cmd, buf, sizeof buf, timeout_ms) != ELM_OK) return false;
    return strchr(buf, '?') == NULL && strstr(buf, "ERROR") == NULL;
}

elm_status_t elm_request(const char *hex, uint8_t *out, size_t max, size_t *n, uint32_t timeout_ms)
{
    char buf[512];
    *n = 0;
    elm_status_t st = elm_cmd(hex, buf, sizeof buf, timeout_ms);
    if (st != ELM_OK) return st;
    return elm_parse_hex(buf, hex, out, max, n);
}
