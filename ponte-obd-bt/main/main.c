// Ponte OBD - ESP32 classico (WROOM-32 / DevKit V1 / NodeMCU-32S)
//
//   ELM327 Bluetooth  <-- SPP (Bluetooth Classic) -->  ESP32  <-- UART 38400 -->  painel ESP32-S3
//
// A ponte e TRANSPARENTE: tudo que o painel manda pela serial vai para o ELM327
// e tudo que o ELM327 responde volta para o painel, byte a byte.
//
// Estados: PARADO -> (BUSCA -> NOMES) -> SDP -> CONECTANDO -> CONECTADO
//   - com MAC configurado (ou salvo na flash) pula a busca
//   - caiu a conexao: espera 3 s e tenta de novo, para sempre
//   - PIN recusado: a proxima tentativa usa o proximo PIN da lista
//   - 12 falhas seguidas com o endereco salvo: esquece e volta a procurar pelo nome

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ponte";

#define UART_PORT       UART_NUM_2
#define UART_BUF        2048
#define MAX_CAND        8
#define RETRY_MS        3000
#define STUCK_MS        25000
#define FORGET_AFTER    12
#define MAX_PINS        8

typedef enum { ST_IDLE, ST_SEARCH, ST_NAMES, ST_SDP, ST_CONNECTING, ST_CONNECTED } state_t;
static const char *st_name[] = { "parado", "busca", "nomes", "sdp", "conectando", "conectado" };

typedef enum {
    EV_DISC_RES,
    EV_DISC_STOP,
    EV_NAME,
    EV_SDP,
    EV_OPEN,
    EV_CLOSE,
    EV_SPP_READY,
    EV_AUTH,
} ev_type_t;

typedef struct {
    ev_type_t type;
    bool ok;
    uint8_t scn;
    uint32_t handle;
    esp_bd_addr_t bda;
    char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
} ev_t;

static QueueHandle_t s_evq;
static state_t s_state = ST_IDLE;
static int64_t s_state_t;
static int64_t s_retry_at;
static bool s_spp_ready;

static esp_bd_addr_t s_target;
static bool s_have_target;
static bool s_target_fixed;
static int s_fail_count;

static esp_bd_addr_t s_cand[MAX_CAND];
static int s_ncand, s_icand;

static volatile uint32_t s_handle;
static volatile bool s_cong;

static char s_pins[MAX_PINS][ESP_BT_PIN_CODE_LEN + 1];
static int s_npins;
static volatile int s_pin_idx;
static volatile bool s_pin_asked;

static void pins_load(void)
{
    const char *p = CONFIG_PONTE_ELM_PINS;
    s_npins = 0;
    while (*p && s_npins < MAX_PINS) {
        size_t n = 0;
        while (*p == ',' || *p == ' ') p++;
        while (*p && *p != ',' && *p != ' ' && n < ESP_BT_PIN_CODE_LEN) s_pins[s_npins][n++] = *p++;
        while (*p && *p != ',') p++;
        s_pins[s_npins][n] = 0;
        if (n) s_npins++;
    }
    if (s_npins == 0) { strcpy(s_pins[0], "1234"); s_npins = 1; }
}

static void next_pin(const char *why)
{
    s_pin_idx = (s_pin_idx + 1) % s_npins;
    ESP_LOGW(TAG, "%s: proxima tentativa com PIN %s", why, s_pins[s_pin_idx]);
}

static int64_t now_us(void) { return esp_timer_get_time(); }

static void set_state(state_t st)
{
    if (st != s_state) ESP_LOGI(TAG, "estado: %s -> %s", st_name[s_state], st_name[st]);
    s_state = st;
    s_state_t = now_us();
    if (CONFIG_PONTE_LINK_GPIO >= 0) gpio_set_level(CONFIG_PONTE_LINK_GPIO, st == ST_CONNECTED);
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "falhou (%s), nova tentativa em %d s", why, RETRY_MS / 1000);
    if (s_have_target && !s_target_fixed && ++s_fail_count >= FORGET_AFTER) {
        ESP_LOGW(TAG, "%d falhas com o endereco salvo: vou procurar pelo nome", s_fail_count);
        s_have_target = false;
        s_fail_count = 0;
    }
    set_state(ST_IDLE);
    s_retry_at = now_us() + (int64_t)RETRY_MS * 1000;
}

static const char *bda_str(const uint8_t *b, char *out)
{
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    return out;
}

static bool parse_mac(const char *s, esp_bd_addr_t out)
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static bool name_matches(const char *name)
{
    if (!name || !name[0]) return false;
    char up[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    size_t i;
    for (i = 0; name[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)name[i]);
    up[i] = 0;

    const char *p = CONFIG_PONTE_ELM_NAMES;
    while (*p) {
        char tok[24];
        size_t n = 0;
        while (*p == ',' || *p == ' ') p++;
        while (*p && *p != ',' && n < sizeof tok - 1) tok[n++] = (char)toupper((unsigned char)*p++);
        while (n && tok[n - 1] == ' ') n--;
        tok[n] = 0;
        if (n && strstr(up, tok)) return true;
    }
    return false;
}

static void target_load(void)
{
    if (parse_mac(CONFIG_PONTE_ELM_MAC, s_target)) {
        s_have_target = s_target_fixed = true;
        return;
    }
    nvs_handle_t h;
    if (nvs_open("ponte", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_target;
    if (nvs_get_blob(h, "bda", s_target, &len) == ESP_OK && len == sizeof s_target) s_have_target = true;
    nvs_close(h);
}

static void target_save(void)
{
    if (s_target_fixed) return;
    nvs_handle_t h;
    if (nvs_open("ponte", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "bda", s_target, sizeof s_target);
    nvs_commit(h);
    nvs_close(h);
}

static void post(const ev_t *e) { xQueueSend(s_evq, e, 0); }

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    ev_t e = { 0 };
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        e.type = EV_DISC_RES;
        memcpy(e.bda, param->disc_res.bda, sizeof e.bda);
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->len > 0) {
                size_t n = p->len < (int)sizeof e.name - 1 ? p->len : sizeof e.name - 1;
                memcpy(e.name, p->val, n);
                e.name[n] = 0;
            } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && !e.name[0]) {
                uint8_t n = 0;
                uint8_t *s = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &n);
                if (!s) s = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &n);
                if (s && n) {
                    if (n > sizeof e.name - 1) n = sizeof e.name - 1;
                    memcpy(e.name, s, n);
                    e.name[n] = 0;
                }
            }
        }
        post(&e);
        break;

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            e.type = EV_DISC_STOP;
            post(&e);
        }
        break;

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        e.type = EV_NAME;
        e.ok = param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS;
        if (e.ok) strlcpy(e.name, (const char *)param->read_rmt_name.rmt_name, sizeof e.name);
        post(&e);
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = { 0 };
        const char *pc = s_pins[s_pin_idx];
        size_t n = strlen(pc);
        memcpy(pin, pc, n);
        s_pin_asked = true;
        ESP_LOGI(TAG, "pedido de PIN: respondendo %s", pc);
        esp_bt_gap_pin_reply(param->pin_req.bda, true, n, pin);
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        e.type = EV_AUTH;
        e.ok = param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS;
        if (e.ok) ESP_LOGI(TAG, "pareado com %s", param->auth_cmpl.device_name);
        else ESP_LOGW(TAG, "pareamento falhou (status %d)", param->auth_cmpl.stat);
        post(&e);
        break;

    default:
        break;
    }
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    ev_t e = { 0 };
    switch (event) {
    case ESP_SPP_INIT_EVT:
        e.type = EV_SPP_READY;
        e.ok = param->init.status == ESP_SPP_SUCCESS;
        post(&e);
        break;

    case ESP_SPP_DISCOVERY_COMP_EVT:
        e.type = EV_SDP;
        e.ok = param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0;
        if (e.ok) e.scn = param->disc_comp.scn[0];
        post(&e);
        break;

    case ESP_SPP_OPEN_EVT:
        e.type = EV_OPEN;
        e.ok = param->open.status == ESP_SPP_SUCCESS;
        e.handle = param->open.handle;
        if (e.ok) { s_cong = false; s_handle = param->open.handle; }
        post(&e);
        break;

    case ESP_SPP_CL_INIT_EVT:
        if (param->cl_init.status != ESP_SPP_SUCCESS) {
            e.type = EV_OPEN;
            e.ok = false;
            post(&e);
        }
        break;

    case ESP_SPP_CLOSE_EVT:
        s_handle = 0;
        e.type = EV_CLOSE;
        post(&e);
        break;

    case ESP_SPP_DATA_IND_EVT:
        uart_write_bytes(UART_PORT, param->data_ind.data, param->data_ind.len);
#if CONFIG_PONTE_LOG_TRAFFIC
        ESP_LOGI(TAG, "ELM> %.*s", param->data_ind.len, (const char *)param->data_ind.data);
#endif
        break;

    case ESP_SPP_CONG_EVT:
        s_cong = param->cong.cong;
        break;

    case ESP_SPP_WRITE_EVT:
        s_cong = param->write.cong;
        break;

    default:
        break;
    }
}

static void start_search(void)
{
    s_ncand = s_icand = 0;
    set_state(ST_SEARCH);
    ESP_LOGI(TAG, "procurando ELM327 (nomes: %s)...", CONFIG_PONTE_ELM_NAMES);
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0) != ESP_OK) fail("busca");
}

static void start_connect(uint8_t scn)
{
    char b[18];
    s_pin_asked = false;
    set_state(ST_CONNECTING);
    ESP_LOGI(TAG, "conectando em %s canal %u", bda_str(s_target, b), scn);
    if (esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER, scn, s_target) != ESP_OK)
        fail("connect");
}

static void start_sdp(void)
{
    if (CONFIG_PONTE_SPP_CHANNEL > 0) { start_connect(CONFIG_PONTE_SPP_CHANNEL); return; }
    set_state(ST_SDP);
    if (esp_spp_start_discovery(s_target) != ESP_OK) start_connect(1);
}

static void begin(void)
{
    if (s_have_target) start_sdp();
    else start_search();
}

static void next_name(void)
{
    while (s_icand < s_ncand) {
        set_state(ST_NAMES);
        if (esp_bt_gap_read_remote_name(s_cand[s_icand]) == ESP_OK) return;
        s_icand++;
    }
    fail("nenhum ELM327 encontrado");
}

static void choose(const uint8_t *bda, const char *name)
{
    char b[18];
    memcpy(s_target, bda, sizeof s_target);
    s_have_target = true;
    s_fail_count = 0;
    ESP_LOGI(TAG, "achei \"%s\" em %s", name, bda_str(bda, b));
}

static void handle_event(const ev_t *e)
{
    char b[18];
    switch (e->type) {
    case EV_SPP_READY:
        s_spp_ready = e->ok;
        if (!e->ok) ESP_LOGE(TAG, "SPP nao inicializou");
        break;

    case EV_DISC_RES:
        if (s_state != ST_SEARCH || s_have_target) break;
        if (e->name[0]) {
            ESP_LOGI(TAG, "visto: %s \"%s\"", bda_str(e->bda, b), e->name);
            if (name_matches(e->name)) {
                choose(e->bda, e->name);
                esp_bt_gap_cancel_discovery();
            }
        } else if (s_ncand < MAX_CAND) {
            for (int i = 0; i < s_ncand; i++)
                if (!memcmp(s_cand[i], e->bda, 6)) return;
            memcpy(s_cand[s_ncand++], e->bda, 6);
            ESP_LOGI(TAG, "visto: %s (sem nome, pergunto depois)", bda_str(e->bda, b));
        }
        break;

    case EV_DISC_STOP:
        if (s_state != ST_SEARCH) break;
        if (s_have_target) start_sdp();
        else next_name();
        break;

    case EV_NAME:
        if (s_state != ST_NAMES) break;
        if (e->ok) {
            ESP_LOGI(TAG, "nome de %s: \"%s\"", bda_str(s_cand[s_icand], b), e->name);
            if (name_matches(e->name)) { choose(s_cand[s_icand], e->name); start_sdp(); break; }
        }
        s_icand++;
        next_name();
        break;

    case EV_SDP:
        if (s_state != ST_SDP) break;
        if (e->ok) start_connect(e->scn);
        else { ESP_LOGW(TAG, "SDP sem resposta, tentando canal 1"); start_connect(1); }
        break;

    case EV_OPEN:
        if (s_state != ST_CONNECTING) break;
        if (e->ok) {
            s_fail_count = 0;
            target_save();
            uart_flush_input(UART_PORT);
            set_state(ST_CONNECTED);
            ESP_LOGI(TAG, "CONECTADO ao ELM327 - ponte ativa");
        } else {
            fail("conexao recusada / fora de alcance");
        }
        break;

    case EV_AUTH:
        if (!e->ok && s_pin_asked) { s_pin_asked = false; next_pin("PIN recusado"); }
        break;

    case EV_CLOSE:
        if (s_state == ST_CONNECTED || s_state == ST_CONNECTING) fail("conexao caiu");
        break;
    }
}

static void link_task(void *arg)
{
    bool led = false;
    int64_t led_t = 0;
    for (;;) {
        ev_t e;
        if (xQueueReceive(s_evq, &e, pdMS_TO_TICKS(50)) == pdTRUE) handle_event(&e);

        int64_t t = now_us();
        if (s_state == ST_IDLE && s_spp_ready && t >= s_retry_at) begin();

        if (s_state != ST_IDLE && s_state != ST_CONNECTED && t - s_state_t > (int64_t)STUCK_MS * 1000) {
            if (s_state == ST_SEARCH) esp_bt_gap_cancel_discovery();
            fail("tempo esgotado");
        }

        if (CONFIG_PONTE_LED_GPIO >= 0) {
            if (s_state == ST_CONNECTED) led = true;
            else if (t - led_t > (s_state == ST_IDLE ? 1000000 : 150000)) { led = !led; led_t = t; }
            gpio_set_level(CONFIG_PONTE_LED_GPIO, led);
        }
    }
}

static void uart_task(void *arg)
{
    uint8_t buf[128];
    for (;;) {
        size_t n = 0;
        int r = uart_read_bytes(UART_PORT, buf, 1, portMAX_DELAY);
        if (r <= 0) continue;
        n = 1;
        while (buf[n - 1] != '\r' && n < sizeof buf) {
            r = uart_read_bytes(UART_PORT, buf + n, 1, pdMS_TO_TICKS(5));
            if (r <= 0) break;
            n++;
        }

        uint32_t h = s_handle;
        if (s_state != ST_CONNECTED || h == 0) continue;

        for (int i = 0; s_cong && i < 40; i++) vTaskDelay(pdMS_TO_TICKS(5));
#if CONFIG_PONTE_LOG_TRAFFIC
        ESP_LOGI(TAG, "PAINEL> %.*s", (int)n, (const char *)buf);
#endif
        if (esp_spp_write(h, n, buf) != ESP_OK) ESP_LOGW(TAG, "falha ao enviar %u bytes", (unsigned)n);
    }
}

static void hw_init(void)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_PONTE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF, UART_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, CONFIG_PONTE_UART_TX, CONFIG_PONTE_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(CONFIG_PONTE_UART_RX);

    if (CONFIG_PONTE_LED_GPIO >= 0) {
        gpio_reset_pin(CONFIG_PONTE_LED_GPIO);
        gpio_set_direction(CONFIG_PONTE_LED_GPIO, GPIO_MODE_OUTPUT);
    }
    if (CONFIG_PONTE_LINK_GPIO >= 0) {
        gpio_reset_pin(CONFIG_PONTE_LINK_GPIO);
        gpio_set_direction(CONFIG_PONTE_LINK_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_PONTE_LINK_GPIO, 0);
    }
    ESP_LOGI(TAG, "UART2 TX=%d RX=%d %d baud", CONFIG_PONTE_UART_TX, CONFIG_PONTE_UART_RX,
             CONFIG_PONTE_UART_BAUD);
}

static void bt_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(spp_cb));

    esp_bt_pin_code_t pin = { 0 };
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof iocap);

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&spp_cfg));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_evq = xQueueCreate(16, sizeof(ev_t));
    pins_load();
    ESP_LOGI(TAG, "PINs a tentar: %s", CONFIG_PONTE_ELM_PINS);
    hw_init();
    target_load();
    if (s_have_target) {
        char b[18];
        ESP_LOGI(TAG, "ELM327 %s: %s", s_target_fixed ? "configurado" : "salvo", bda_str(s_target, b));
    }
    bt_init();

    xTaskCreatePinnedToCore(link_task, "link", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(uart_task, "uart", 3072, NULL, 7, NULL, 1);
    ESP_LOGI(TAG, "ponte iniciada");
}
