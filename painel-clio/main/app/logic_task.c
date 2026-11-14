// logic_task.c - computador de bordo (10 Hz)
// Junta os dados da ECU, integra distancia/combustivel, detecta motor
// ligado/desligado, marcha, conselho de troca, monitor de injecao e salva na NVS.
#include <math.h>
#include <string.h>
#include "app_state.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "storage.h"

static const char *TAG = "logic";

#define PERIOD_MS          100
#define SAVE_EVERY_S       60      // grava a viagem na flash a cada 60 s com motor ligado
#define ENGINE_ON_RPM      400
#define ENGINE_OFF_S       2.0f    // rpm 0 (ou ECU muda) por 2 s = motor desligou
#define OFFLINE_OFF_S      3.0f
#define POWER_OFF_AFTER_S  45.0f   // com power-hold: quanto tempo mostra o relatorio

static trip_state_t s_trip;
static injmon_t s_inj;
static shift_state_t s_shift;
static gear_learn_acc_t s_learn;
static int s_learn_gear = 0;
static float s_learn_left_s = 0;

static void ev_add(ev_type_t type, const vehicle_sample_t *s)
{
    ev_t ev = { .t_s = (uint32_t)(esp_timer_get_time() / 1000000), .type = (uint8_t)type };
    ev.tps = VALID(s->tps_pct) ? (uint8_t)s->tps_pct : 0;
    ev.rpm = VALID(s->rpm) ? (uint16_t)s->rpm : 0;
    ev.speed = VALID(s->speed_kmh) ? (uint8_t)s->speed_kmh : 0;
    ev.rco = VALID(s->idle_rco) ? (uint8_t)s->idle_rco : 255;
    app_lock();
    g_ev.e[g_ev.head] = ev;
    g_ev.head = (g_ev.head + 1) % MAX_EVENTS;
    if (g_ev.n < MAX_EVENTS) g_ev.n++;
    g_ev.seq++;
    app_unlock();
    ESP_LOGI(TAG, "evento: %s | borboleta %u%% %u rpm %u km/h valvula ralenti %d%% (-1 = ECU nao informa)",
             g_ev_names[type], ev.tps, ev.rpm, ev.speed, ev.rco == 255 ? -1 : (int)ev.rco);
}

// Pressostato da direcao (nivel eletrico) e queda de rotacao. O mesmo registro do A/C.
static void steer_track(const vehicle_sample_t *s, bool fresh)
{
    static int last_ps = -1;
    static float prev_rpm = 0;
    static bool dropped = false;
    if (!fresh) return;
    if (VALID(s->ps_level)) {
        int p = s->ps_level > 0.5f;
        if (last_ps >= 0 && p != last_ps) ev_add(p ? EV_PS_HI : EV_PS_LO, s);
        last_ps = p;
    }
    if (VALID(s->rpm)) {
        if (prev_rpm >= 650.0f && s->rpm < 450.0f && !dropped) { ev_add(EV_RPM_DROP, s); dropped = true; }
        if (s->rpm >= 650.0f) dropped = false;
        prev_rpm = s->rpm;
    }
}

// Detecta mudancas do pedido e do comando do compressor. Os dois vem de requisicoes
// diferentes (21A2 e 21A4), entao a resolucao no tempo e de ~1 s: corte muito curto pode passar.
static void ac_track(const vehicle_sample_t *s, bool fresh)
{
    static int last_req = -1, last_cmd = -1;
    if (!fresh) return;
    if (VALID(s->ac_req)) {
        int r = s->ac_req > 0.5f;
        if (last_req >= 0 && r != last_req) ev_add(r ? EV_AC_REQ_ON : EV_AC_REQ_OFF, s);
        last_req = r;
    }
    if (VALID(s->ac_cmd)) {
        int c = s->ac_cmd > 0.5f;
        if (last_cmd >= 0 && c != last_cmd) {
            if (c) ev_add(EV_AC_ON, s);
            else ev_add(last_req > 0 ? EV_AC_CUT : EV_AC_OFF, s);
        }
        last_cmd = c;
    }
}

static void power_pins_init(void)
{
#if CONFIG_PAINEL_POWER_HOLD_GPIO >= 0
    gpio_reset_pin(CONFIG_PAINEL_POWER_HOLD_GPIO);
    gpio_set_direction(CONFIG_PAINEL_POWER_HOLD_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_PAINEL_POWER_HOLD_GPIO, 1);    // segura a alimentacao
#endif
#if CONFIG_PAINEL_IGN_SENSE_GPIO >= 0
    gpio_reset_pin(CONFIG_PAINEL_IGN_SENSE_GPIO);
    gpio_set_direction(CONFIG_PAINEL_IGN_SENSE_GPIO, GPIO_MODE_INPUT);
#endif
}

static bool ignition_on(void)
{
#if CONFIG_PAINEL_IGN_SENSE_GPIO >= 0
    return gpio_get_level(CONFIG_PAINEL_IGN_SENSE_GPIO) == 1;
#else
    return true;
#endif
}

static void power_release(void)
{
#if CONFIG_PAINEL_POWER_HOLD_GPIO >= 0
    ESP_LOGI(TAG, "desligando a alimentacao");
    gpio_set_level(CONFIG_PAINEL_POWER_HOLD_GPIO, 0);
#endif
}

static void handle_cmds(void)
{
    logic_msg_t m;
    while (logic_get(&m)) {
        app_lock();
        switch (m.cmd) {
        case LCMD_RESET_AVG:
            trip_reset_average(&s_trip);
            storage_save_trip(&s_trip.p);
            break;
        case LCMD_FILL_TANK:
            trip_fill_tank(&s_trip);
            storage_save_trip(&s_trip.p);
            break;
        case LCMD_CALIBRATE:
            g_cfg.fuel_cal = (float)trip_calibrate_factor(&s_trip, m.arg, g_cfg.fuel_cal);
            ESP_LOGI(TAG, "novo fator de consumo: %.3f", g_cfg.fuel_cal);
            trip_fill_tank(&s_trip);    // calibrou = acabou de completar o tanque
            storage_save_settings(&g_cfg);
            storage_save_trip(&s_trip.p);
            break;
        case LCMD_LEARN_GEAR:
            memset(&s_learn, 0, sizeof s_learn);
            s_learn_gear = (int)m.arg;
            s_learn_left_s = 8.0f;      // 8 s de amostras
            break;
        case LCMD_SAVE_SETTINGS:
            storage_save_settings(&g_cfg);
            break;
        case LCMD_REPORT_SEEN:
            g_dash.show_report = false;
            break;
        case LCMD_CLEAR_TQ:
            memset(g_cfg.tq_curve, 0, sizeof g_cfg.tq_curve);
            storage_save_settings(&g_cfg);
            break;
        }
        app_unlock();
    }
}

static void logic_task(void *arg)
{
    float off_s = 0, offline_s = 0, on_s = 0, since_save = 0, report_age = 0, inj_save = 0;
    uint32_t last_seq = 0;
    int last_gear = 0, gear_cand = 0;
    float gear_cand_s = 0;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
        const float dt = PERIOD_MS / 1000.0f;
        handle_cmds();

        app_lock();
        vehicle_sample_t s = g_obd.s;
        settings_t cfg = g_cfg;
        app_unlock();
        if (cfg.demo) {     // demonstracao: cambio JB3 fixo, sem mexer no que foi aprendido
            memset(cfg.gear_ratio, 0, sizeof cfg.gear_ratio);
            memcpy(cfg.gear_ratio, GEAR_JB3_RPM_PER_KMH, sizeof GEAR_JB3_RPM_PER_KMH);
            cfg.ngears = GEAR_JB3_N;
        }

        bool fresh = s.seq != last_seq;
        last_seq = s.seq;
        bool online = s.ecu_online;
        float rpm = VALID(s.rpm) ? s.rpm : 0;
        if (online) { ac_track(&s, fresh); steer_track(&s, fresh); }

        // ---- motor ligado / desligado
        if (!s_trip.engine_running) {
            on_s = (online && rpm > ENGINE_ON_RPM) ? on_s + dt : 0;
            if (on_s > 1.0f) {
                trip_start(&s_trip);
                app_lock();
                g_dash.show_report = false;
                app_unlock();
                ESP_LOGI(TAG, "motor ligou: viagem nova");
            }
        } else {
            off_s = (online && rpm < 100) ? off_s + dt : 0;
            offline_s = online ? 0 : offline_s + dt;
            if (off_s > ENGINE_OFF_S || offline_s > OFFLINE_OFF_S || !ignition_on()) {
                trip_end(&s_trip);
                storage_save_trip(&s_trip.p);
                storage_save_injmap(&s_inj.map);
                app_lock();
                g_dash.show_report = s_trip.p.last_trip.dist_km > 0.05;
                g_dash.report_seq++;
                app_unlock();
                report_age = 0;
                off_s = offline_s = 0;
                ESP_LOGI(TAG, "motor desligou: %.2f km, %.2f L", s_trip.p.last_trip.dist_km,
                         s_trip.p.last_trip.fuel_l);
            }
        }

        // ---- consumo e integracao (so com dado recente: nao inventa durante falhas)
        fuel_src_t fsrc = FUEL_SRC_NONE;
        float lph = fuel_lph(&s, &cfg, &fsrc);
        float v = VALID(s.speed_kmh) ? s.speed_kmh * cfg.dist_cal : VNAN;
        static float stale_s = 0;
        stale_s = fresh ? 0 : stale_s + dt;
        if (s_trip.engine_running && online && stale_s < 1.5f) trip_update(&s_trip, v, lph, dt);

        // ---- marcha (com filtro: precisa ficar 0,4 s igual)
        int g = gear_detect(&cfg, s.rpm, v);
        if (g == gear_cand) gear_cand_s += dt; else { gear_cand = g; gear_cand_s = 0; }
        if (gear_cand_s > 0.4f) last_gear = gear_cand;

        // ---- aprender marcha
        if (s_learn_gear > 0) {
            gear_learn_add(&s_learn, s.rpm, v);
            s_learn_left_s -= dt;
            if (s_learn_left_s <= 0) {
                app_lock();
                bool ok = gear_learn_commit(&g_cfg, s_learn_gear, &s_learn);
                if (ok) storage_save_settings(&g_cfg);
                app_unlock();
                ESP_LOGI(TAG, "marcha %d: %s (%.1f rpm/kmh, %d amostras)", s_learn_gear,
                         ok ? "aprendida" : "FALHOU", s_learn.n ? s_learn.sum / s_learn.n : 0, s_learn.n);
                s_learn_gear = 0;
            }
        }

        shift_advice_t adv = shift_update(&s_shift, &cfg, &s, last_gear, dt);
        static shift_advice_t last_adv = SHIFT_NONE;
        if (adv != last_adv) {
            last_adv = adv;
            ESP_LOGI(TAG, "troca: %s (marcha %d, %.0f rpm)",
                     adv == SHIFT_UP ? "SUBIR" : adv == SHIFT_DOWN ? "REDUZIR" : "-", last_gear, rpm);
        }
        injmon_status_t ist = fresh ? injmon_update(&s_inj, &s, dt) : s_inj.status;
        if (fresh && !cfg.demo) {    // a simulacao nao pode gravar curva de torque falsa
            app_lock();
            torque_learn(&g_cfg, &s);
            app_unlock();
        }

        // ---- salvar periodicamente
        if (s_trip.engine_running) {
            since_save += dt;
            inj_save += dt;
            if (since_save > SAVE_EVERY_S) { storage_save_trip(&s_trip.p); since_save = 0; }
            if (inj_save > 600) {
                storage_save_injmap(&s_inj.map);
                app_lock();
                storage_save_settings(&g_cfg);      // curva de torque aprendida
                app_unlock();
                inj_save = 0;
            }
        }

        // ---- desligar a alimentacao depois do relatorio (se houver o circuito)
        if (!s_trip.engine_running && !ignition_on()) {
            report_age += dt;
            if (report_age > POWER_OFF_AFTER_S) power_release();
        }

        // ---- publicar para a tela
        double fuel_left = 0;
        double fallback = 11.0;   // km/L antes de ter media (so para nao mostrar vazio)
        double range = trip_range_km(&s_trip, cfg.tank_l, fallback, &fuel_left);
        app_lock();
        g_dash.s = s;
        g_dash.lph = lph;
        g_dash.fuel_src = fsrc;
        g_dash.inst = fuel_instant_display(lph, v, cfg.units_kml);
        g_dash.avg_trip = trip_kml(&s_trip.current);
        g_dash.avg_reset = trip_kml(&s_trip.p.since_reset);
        g_dash.range_km = range;
        g_dash.fuel_left_l = fuel_left;
        g_dash.current = s_trip.current;
        g_dash.last_trip = s_trip.p.last_trip;
        g_dash.engine_running = s_trip.engine_running;
        g_dash.gear = last_gear;
        g_dash.shift = adv;
        g_dash.inj_status = ist;
        g_dash.inj_dev_pct = s_inj.dev_pct;
        g_dash.sport_up_rpm = last_gear ? shift_optimal_upshift_rpm(&g_cfg, last_gear) : VNAN;
        g_dash.learning_gear = s_learn_gear > 0;
        g_dash.learn_gear_n = s_learn_gear;
        g_dash.learn_samples = s_learn.n;
        app_unlock();
    }
}

void logic_task_start(void)
{
    trip_init(&s_trip);
    storage_load_trip(&s_trip.p);
    injmon_init(&s_inj);
    storage_load_injmap(&s_inj.map);
    power_pins_init();
    xTaskCreatePinnedToCore(logic_task, "logic", 6144, NULL, 4, NULL, 0);
}
