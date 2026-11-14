#include "storage.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs";
static nvs_handle_t s_h;

void storage_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS formatada");
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(nvs_open("painel", NVS_READWRITE, &s_h));
}

static bool load(const char *key, void *buf, size_t size)
{
    size_t len = size;
    esp_err_t e = nvs_get_blob(s_h, key, buf, &len);
    if (e != ESP_OK || len != size) {   // tamanho mudou = versao nova do struct: descarta
        ESP_LOGW(TAG, "%s: %s", key, e == ESP_OK ? "tamanho diferente" : esp_err_to_name(e));
        return false;
    }
    return true;
}

static void save(const char *key, const void *buf, size_t size)
{
    esp_err_t e = nvs_set_blob(s_h, key, buf, size);
    if (e == ESP_OK) e = nvs_commit(s_h);
    if (e != ESP_OK) ESP_LOGE(TAG, "salvar %s: %s", key, esp_err_to_name(e));
}

bool storage_load_settings(settings_t *s)
{
    settings_t tmp;
    if (!load("cfg", &tmp, sizeof tmp) || tmp.magic != SETTINGS_MAGIC || tmp.version != SETTINGS_VERSION)
        return false;
    *s = tmp;
    return true;
}
void storage_save_settings(const settings_t *s) { save("cfg", s, sizeof *s); }

bool storage_load_trip(trip_persist_t *t)
{
    trip_persist_t tmp;
    if (!load("trip", &tmp, sizeof tmp) || tmp.magic != 0x54524950u) return false;
    *t = tmp;
    return true;
}
void storage_save_trip(const trip_persist_t *t) { save("trip", t, sizeof *t); }

bool storage_load_injmap(injmon_map_t *m) { return load("inj", m, sizeof *m); }
void storage_save_injmap(const injmon_map_t *m) { save("inj", m, sizeof *m); }
