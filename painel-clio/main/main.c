// Painel Clio/Twingo - ESP32-8048S070C
//   core 0: tarefa OBD (ELM327) + tarefa de logica (computador de bordo)
//   core 1: LVGL (tela)
#include "app_state.h"
#include "display.h"
#include "esp_log.h"
#include "obd_task.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    storage_init();
    app_state_init();

    settings_defaults(&g_cfg);
    if (!storage_load_settings(&g_cfg)) {
        ESP_LOGW(TAG, "sem configuracao salva: usando padrao");
        storage_save_settings(&g_cfg);
    }

    display_init();
    display_set_brightness(g_cfg.brightness);
    ui_init();

    logic_task_start();
    obd_task_start();
    ESP_LOGI(TAG, "painel iniciado");
}
