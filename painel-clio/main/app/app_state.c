#include "app_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

obd_shared_t g_obd;
dash_t g_dash;
settings_t g_cfg;
ev_log_t g_ev;
const char *const g_ev_names[EV_TYPES] = {
    "pedido A/C ligou", "pedido A/C desligou", "compressor LIGOU", "compressor DESLIGOU",
    "CORTE do compressor", "pressostato ATIVO", "pressostato inativo", "ROTACAO CAIU",
};

static SemaphoreHandle_t s_mtx;
static QueueHandle_t s_obd_q, s_logic_q;

void app_state_init(void)
{
    s_mtx = xSemaphoreCreateRecursiveMutex();
    s_obd_q = xQueueCreate(4, sizeof(obd_cmd_t));
    s_logic_q = xQueueCreate(8, sizeof(logic_msg_t));
    vehicle_sample_clear(&g_obd.s);
    vehicle_sample_clear(&g_dash.s);
}

void app_lock(void) { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
void app_unlock(void) { xSemaphoreGiveRecursive(s_mtx); }

void obd_post_cmd(obd_cmd_t c) { xQueueSend(s_obd_q, &c, 0); }
bool obd_get_cmd(obd_cmd_t *c) { return xQueueReceive(s_obd_q, c, 0) == pdTRUE; }

void logic_post(logic_cmd_t c, float arg)
{
    logic_msg_t m = { c, arg };
    xQueueSend(s_logic_q, &m, 0);
}
bool logic_get(logic_msg_t *m) { return xQueueReceive(s_logic_q, m, 0) == pdTRUE; }
