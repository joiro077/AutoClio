// sim_main.c - roda o painel no PC (Windows) em modo demonstracao e salva um
// BMP de cada tela. Nao tem janela: serve so para gerar as imagens do README.
//
// Compila o codigo de verdade (ui.c, obd_task.c, logic_task.c, core/*.c) contra
// o LVGL do projeto. So o que depende do chip e trocado por imitacoes: FreeRTOS
// (threads do Windows), UART do ELM327, NVS e o painel LCD.
//
// Uso: painel_sim.exe <pasta de saida>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_rt.h"
#include "elm327.h"
#include "storage.h"

// ui.c entra aqui inteiro para o simulador enxergar as telas (sao "static").
#include "../../main/app/ui.c"

#define W 800
#define H 480
#define SPEED 2          // o tempo simulado anda SPEED vezes mais rapido que o real

/* ------------------------------------------------------------ tempo simulado */
static LARGE_INTEGER s_qpf, s_qp0;
int64_t esp_timer_get_time(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (int64_t)((double)(n.QuadPart - s_qp0.QuadPart) * 1e6 * SPEED / (double)s_qpf.QuadPart);
}
static uint32_t sim_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void sim_sleep_until(uint32_t t_ms)
{
    for (;;) {
        int32_t left = (int32_t)(t_ms - sim_ms());
        if (left <= 0) return;
        int real = left / SPEED;
        Sleep(real > 0 ? (DWORD)real : 0);
    }
}

/* ------------------------------------------------------------ FreeRTOS */
typedef struct { void (*fn)(void *); void *arg; } task_start_t;
static DWORD WINAPI task_tramp(LPVOID p)
{
    task_start_t t = *(task_start_t *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   int prio, TaskHandle_t *h, int core)
{
    (void)name; (void)stack; (void)prio; (void)core;
    task_start_t *t = malloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    HANDLE th = CreateThread(NULL, 1 << 20, task_tramp, t, 0, NULL);
    if (h) *h = th;
    return pdPASS;
}
TickType_t xTaskGetTickCount(void) { return sim_ms(); }
void vTaskDelay(TickType_t t) { sim_sleep_until(sim_ms() + t); }
void vTaskDelayUntil(TickType_t *prev, TickType_t inc) { *prev += inc; sim_sleep_until(*prev); }

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    CRITICAL_SECTION *cs = malloc(sizeof *cs);
    InitializeCriticalSection(cs);
    return cs;
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t to) { (void)to; EnterCriticalSection(m); return pdTRUE; }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m) { LeaveCriticalSection(m); return pdTRUE; }

typedef struct { CRITICAL_SECTION cs; int len, isz, n, head; uint8_t *buf; } simq_t;
QueueHandle_t xQueueCreate(int len, int item_size)
{
    simq_t *q = calloc(1, sizeof *q);
    InitializeCriticalSection(&q->cs);
    q->len = len; q->isz = item_size; q->buf = malloc((size_t)len * item_size);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t to)
{
    (void)to;
    simq_t *q = h;
    BaseType_t ok = pdFALSE;
    EnterCriticalSection(&q->cs);
    if (q->n < q->len) {
        memcpy(q->buf + (size_t)((q->head + q->n) % q->len) * q->isz, item, q->isz);
        q->n++; ok = pdTRUE;
    }
    LeaveCriticalSection(&q->cs);
    return ok;
}
BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t to)
{
    (void)to;
    simq_t *q = h;
    BaseType_t ok = pdFALSE;
    EnterCriticalSection(&q->cs);
    if (q->n > 0) {
        memcpy(item, q->buf + (size_t)q->head * q->isz, q->isz);
        q->head = (q->head + 1) % q->len; q->n--; ok = pdTRUE;
    }
    LeaveCriticalSection(&q->cs);
    return ok;
}

/* ------------------------------------------------------------ ELM327 e NVS (nao existem no PC) */
esp_err_t elm_init(int u, int tx, int rx, int baud) { (void)u; (void)tx; (void)rx; (void)baud; return ESP_OK; }
elm_status_t elm_cmd(const char *c, char *r, size_t m, uint32_t t) { (void)c; (void)t; if (m) r[0] = 0; return ELM_TIMEOUT; }
bool elm_at(const char *c, uint32_t t) { (void)c; (void)t; return false; }
elm_status_t elm_request(const char *h, uint8_t *o, size_t m, size_t *n, uint32_t t)
{ (void)h; (void)o; (void)m; (void)t; if (n) *n = 0; return ELM_TIMEOUT; }

void storage_init(void) {}
bool storage_load_settings(settings_t *s) { (void)s; return false; }
void storage_save_settings(const settings_t *s) { (void)s; }
bool storage_load_trip(trip_persist_t *t) { (void)t; return false; }
void storage_save_trip(const trip_persist_t *t) { (void)t; }
bool storage_load_injmap(injmon_map_t *m) { (void)m; return false; }
void storage_save_injmap(const injmon_map_t *m) { (void)m; }

/* ------------------------------------------------------------ tela */
static CRITICAL_SECTION s_lv_cs;
static uint16_t s_fb[W * H];
void display_init(void) {}
void display_set_brightness(int pct) { (void)pct; }
bool display_lock(void) { EnterCriticalSection(&s_lv_cs); return true; }
void display_unlock(void) { LeaveCriticalSection(&s_lv_cs); }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) { (void)a; (void)px; lv_display_flush_ready(d); }
static uint32_t tick_cb(void) { return sim_ms(); }

// Roda o LVGL por "ms" milissegundos simulados.
static void run_ms(uint32_t ms)
{
    uint32_t end = sim_ms() + ms;
    while ((int32_t)(end - sim_ms()) > 0) {
        display_lock();
        lv_timer_handler();
        display_unlock();
        Sleep(2);
    }
}

static void save_bmp(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s\\%s.bmp", dir, name);
    display_lock();
    lv_refr_now(NULL);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t row = W * 3, size = 54 + row * H;
        uint8_t hd[54] = { 'B', 'M' };
        memcpy(hd + 2, &size, 4);
        uint32_t v = 54; memcpy(hd + 10, &v, 4);
        v = 40; memcpy(hd + 14, &v, 4);
        int32_t w = W, h = -H; memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4);   // altura negativa = de cima para baixo
        uint16_t p = 1; memcpy(hd + 26, &p, 2);
        p = 24; memcpy(hd + 28, &p, 2);
        fwrite(hd, 1, 54, f);
        static uint8_t line[W * 3];
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint16_t c = s_fb[y * W + x];
                uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                line[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
                line[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
                line[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
            }
            fwrite(line, 1, sizeof line, f);
        }
        fclose(f);
        printf("salvo %s (t = %.1f s)\n", path, sim_ms() / 1000.0);
    } else {
        printf("ERRO ao gravar %s\n", path);
    }
    display_unlock();
}

// Abre a tela pelo mesmo caminho do botao, espera ela se atualizar e salva.
static void shot(const char *dir, const char *name, void (*open)(lv_event_t *))
{
    display_lock();
    open(NULL);
    display_unlock();
    run_ms(700);
    save_bmp(dir, name);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_qp0);
    InitializeCriticalSection(&s_lv_cs);

    // mesma ordem do app_main() de verdade
    storage_init();
    app_state_init();
    settings_defaults(&g_cfg);
    g_cfg.demo = 1;

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof s_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    ui_init();
    logic_task_start();
    obd_task_start();

    // O ciclo da demonstracao dura 120 s "do carro" e roda a 2x: 60 s simulados.
    // Os horarios abaixo estao em segundos simulados desde a partida.
    char name[64];
    static const uint32_t rounds[] = { 14000, 24000, 34000, 44000 };
    for (int i = 0; i < 4; i++) {
        while (sim_ms() < rounds[i]) run_ms(100);
        snprintf(name, sizeof name, "painel-%d", i);  shot(dir, name, ev_go_dash);
        snprintf(name, sizeof name, "direcao-%d", i); shot(dir, name, ev_go_steer);
        snprintf(name, sizeof name, "mistura-%d", i); shot(dir, name, ev_go_mix);
        snprintf(name, sizeof name, "eventos-%d", i); shot(dir, name, ev_go_ev);
        snprintf(name, sizeof name, "dados-%d", i);   shot(dir, name, ev_go_data);
        display_lock(); ev_go_dash(NULL); display_unlock();
    }
    shot(dir, "menu", ev_go_menu);
    shot(dir, "falhas", ev_go_dtc);

    // Relatorio de viagem: aparece sozinho quando o motor "desliga" no fim do ciclo.
    display_lock(); ev_go_dash(NULL); display_unlock();
    while (sim_ms() < 130000) {
        run_ms(100);
        app_lock();
        bool rep = g_dash.show_report;
        app_unlock();
        if (rep) { run_ms(1000); save_bmp(dir, "relatorio"); break; }
    }
    return 0;
}
