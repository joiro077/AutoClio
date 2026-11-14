// obd_task.c - le a ECU pelo ELM327
//
// Ordem de tentativa (modo AUTO):
//   1) Renault KWP2000 na K-Line, endereco 0x7A (o mesmo que o ddt4all usa):
//      requisicoes 21xx que trazem varios valores por resposta, inclusive o
//      consumo instantaneo calculado pela propria ECU e o tempo de injecao.
//   2) OBD2 padrao (SAE J1979) - o mesmo do Car Scanner "OBD-II / EOBD".
//
// So existe UMA linha K: enquanto o painel estiver lendo, nao use o Car
// Scanner/ddt4all ao mesmo tempo (as mensagens colidem).
#include "obd_task.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "driver/uart.h"
#include "elm327.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "obd2_decode.h"
#include "sdkconfig.h"

static const char *TAG = "obd";

typedef enum { M_NONE = 0, M_RENAULT, M_OBD2 } link_mode_t;

static link_mode_t s_mode = M_NONE;
static int s_profile = -1;
static obd2_supported_t s_sup;
static bool s_count_suffix = true;   // "21A9 1": avisa o ELM que vem 1 resposta (mais rapido)
static int s_fail = 0;
static int s_ok_count = 0;
static int64_t s_rate_t0 = 0;
static volatile bool s_mix_focus = false;

void obd_set_mix_focus(bool on) { s_mix_focus = on; }
static volatile bool s_steer_focus = false;
void obd_set_steer_focus(bool on) { s_steer_focus = on; }

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void set_status(const char *fmt, ...)
{
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    app_lock();
    strncpy(g_obd.status, tmp, sizeof g_obd.status - 1);
    app_unlock();
    ESP_LOGI(TAG, "%s", tmp);
}

static void publish(const vehicle_sample_t *s)
{
    app_lock();
    uint32_t seq = g_obd.s.seq;      // a copia abaixo zeraria o contador
    g_obd.s = *s;
    g_obd.s.t_ms = now_ms();
    g_obd.s.seq = seq + 1;
    g_obd.s.ecu_online = true;
    app_unlock();
}

static void set_offline(void)
{
    app_lock();
    g_obd.s.ecu_online = false;
    app_unlock();
}

static void count_rate(void)
{
    s_ok_count++;
    int64_t t = esp_timer_get_time();
    if (t - s_rate_t0 > 2000000) {
        float r = s_ok_count * 1e6f / (float)(t - s_rate_t0);
        app_lock();
        g_obd.req_per_s = r;
        app_unlock();
        s_ok_count = 0;
        s_rate_t0 = t;
    }
}

// ---------------------------------------------------------------- ELM setup
static bool elm_setup(void)
{
    char buf[96];
    elm_cmd("ATZ", buf, sizeof buf, 3000);          // reset (resposta: "ELM327 v1.5")
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!elm_at("ATE0", 1000)) return false;        // sem eco
    elm_at("ATL0", 500);                            // sem line feed
    elm_at("ATS0", 500);                            // sem espacos (resposta menor = mais rapido)
    elm_at("ATH0", 500);                            // sem cabecalho
    elm_at("ATAT1", 500);                           // timing adaptativo
    elm_at("ATST32", 500);                          // timeout 0x32*4 = 200 ms
    if (elm_cmd("ATI", buf, sizeof buf, 1000) == ELM_OK) ESP_LOGI(TAG, "adaptador: %s", buf);
    return true;
}

static elm_status_t req(const char *hex, uint8_t *r, size_t max, size_t *n, uint32_t to)
{
    char cmd[24];
    if (s_count_suffix) snprintf(cmd, sizeof cmd, "%s1", hex);
    else snprintf(cmd, sizeof cmd, "%s", hex);
    elm_status_t st = elm_request(cmd, r, max, n, to);
    if (st == ELM_UNKNOWN_CMD && s_count_suffix) {
        ESP_LOGW(TAG, "ELM nao aceita contagem de respostas; desligando");
        s_count_suffix = false;
        st = elm_request(hex, r, max, n, to);
    }
    return st;
}

// ---------------------------------------------------------------- Renault
static bool connect_renault(void)
{
    // Sem perfil ainda: tenta 5 baud primeiro. Todas as Sirius 32 (EMS3132) do
    // ddt4all, inclusive a Sirius 32N do Clio K4M, usam init a 5 baud.
    bool fast_first = (s_profile >= 0) ? g_ecu_profiles[s_profile].fastinit : false;
    for (int attempt = 0; attempt < 2; attempt++) {
        bool fast = (attempt == 0) ? fast_first : !fast_first;
        set_status("Conectando Renault 0x7A (%s)...", fast ? "init rapido" : "init 5 baud");
        elm_at("ATPC", 500);
        elm_at(fast ? "ATSP5" : "ATSP4", 500);     // ISO 14230-4 KWP fast / 5 baud
        elm_at("ATSH817AF1", 500);                  // destino 0x7A (injecao), origem F1 (testador)
        elm_at("ATIIA7A", 500);                     // endereco do init 5 baud
        elm_at("ATWM817AF13E", 500);                // keep-alive: TesterPresent
        uint8_t r[64];
        size_t n = 0;
        elm_status_t st = elm_request("2180", r, sizeof r, &n, 8000);  // 1o pedido faz o init
        if (st == ELM_NEGATIVE) {
            // algumas pedem sessao de diagnostico antes (o ddt4all manda 10C0 ou 10A0)
            elm_request("10C0", r, sizeof r, &n, 2000);
            st = elm_request("2180", r, sizeof r, &n, 2000);
            if (st == ELM_NEGATIVE) {
                elm_request("10A0", r, sizeof r, &n, 2000);
                st = elm_request("2180", r, sizeof r, &n, 2000);
            }
        }
        if (st != ELM_OK) {
            ESP_LOGW(TAG, "Renault: %s", elm_status_str(st));
            continue;
        }
        ecu_id_info_t id;
        if (!kwp_parse_ident(r, n, &id)) continue;
        int p = (g_cfg.ecu_profile >= 0 && g_cfg.ecu_profile < g_ecu_profile_count)
                    ? g_cfg.ecu_profile : kwp_match_profile(&id);
        app_lock();
        g_obd.ecu_id = id;
        if (p >= 0) snprintf(g_obd.ecu_name, sizeof g_obd.ecu_name, "%s", g_ecu_profiles[p].name);
        else snprintf(g_obd.ecu_name, sizeof g_obd.ecu_name, "Desconhecida %s/%s/%s",
                      id.supplier, id.soft, id.version);
        app_unlock();
        if (p < 0) {
            // ECU respondeu mas nao temos o mapa de bytes dela: nao chuta, cai pro OBD2
            ESP_LOGW(TAG, "ECU sem perfil: fornecedor=%s soft=%s versao=%s diag=%d",
                     id.supplier, id.soft, id.version, id.diag);
            return false;
        }
        s_profile = p;
        set_status("Renault KWP: %s", g_ecu_profiles[p].name);
        return true;
    }
    return false;
}

static void poll_renault(vehicle_sample_t *s)
{
    const ecu_profile_t *p = &g_ecu_profiles[s_profile];
    static int rr = 0;
    static bool rpm_turn = true;
    int rpm_lid = kwp_rpm_lid(p);
    uint8_t others[6];
    int no = 0;
    for (int i = 0; i < p->nlids; i++)
        if (p->lids[i] != rpm_lid) others[no++] = p->lids[i];
    if (s_steer_focus) {       // so as requisicoes que trazem pressostato, valvula e consigna
        uint8_t f[6];
        int nf = 0;
        for (size_t k = 0; k < p->nfields; k++) {
            ecu_field_id_t id = p->fields[k].id;
            uint8_t l = p->fields[k].lid;
            if ((id != F_PS_LEVEL && id != F_IDLE_RCO && id != F_IDLE_SET) || l == rpm_lid) continue;
            bool dup = false;
            for (int j = 0; j < nf; j++) if (f[j] == l) dup = true;
            if (!dup && nf < 6) f[nf++] = l;
        }
        if (nf) { memcpy(others, f, (size_t)nf); no = nf; }
    }

    uint8_t lid = (rpm_turn || no == 0) ? (uint8_t)rpm_lid : others[rr++ % no];
    rpm_turn = !rpm_turn;

    char cmd[8];
    snprintf(cmd, sizeof cmd, "21%02X", lid);
    uint8_t r[64];
    size_t n = 0;
    elm_status_t st = req(cmd, r, sizeof r, &n, 1000);
    if (st == ELM_OK && kwp_decode_frame(p, r, n, s) > 0) {
        s->source = SRC_RENAULT;
        publish(s);
        count_rate();
        s_fail = 0;
    } else {
        s_fail++;
        ESP_LOGD(TAG, "%s -> %s", cmd, elm_status_str(st));
    }
}

// ---------------------------------------------------------------- OBD2
static bool connect_obd2(void)
{
    static const char *protos[] = { "ATSP5", "ATSP4", "ATSP3" };   // KWP fast, KWP 5 baud, ISO 9141
    static const char *hdrs[] = { "ATSHC133F1", "ATSHC133F1", "ATSH686AF1" };
    for (int i = 0; i < 3; i++) {
        set_status("Conectando OBD2 (%s)...", protos[i] + 4);
        elm_at("ATPC", 500);
        elm_at(protos[i], 500);
        elm_at(hdrs[i], 500);
        uint8_t r[64];
        size_t n;
        if (elm_request("0100", r, sizeof r, &n, 8000) != ELM_OK) continue;
        memset(&s_sup, 0, sizeof s_sup);
        obd2_parse_supported(r, n, &s_sup);
        if (obd2_is_supported(&s_sup, 0x20) && elm_request("0120", r, sizeof r, &n, 1000) == ELM_OK)
            obd2_parse_supported(r, n, &s_sup);
        if (obd2_is_supported(&s_sup, 0x40) && elm_request("0140", r, sizeof r, &n, 1000) == ELM_OK)
            obd2_parse_supported(r, n, &s_sup);
        ESP_LOGI(TAG, "PIDs suportados: %08lX %08lX %08lX", (unsigned long)s_sup.mask[0],
                 (unsigned long)s_sup.mask[1], (unsigned long)s_sup.mask[2]);
        set_status("OBD2 conectado (%s)", protos[i] + 4);
        app_lock();
        snprintf(g_obd.ecu_name, sizeof g_obd.ecu_name, "OBD2 generico");
        app_unlock();
        return true;
    }
    return false;
}

static void poll_obd2(vehicle_sample_t *s)
{
    // RPM a cada 2 pedidos; o resto em rodizio
    static const uint8_t slow[] = { 0x0D, 0x0B, 0x11, 0x0D, 0x05, 0x04, 0x0D, 0x0F, 0x06, 0x07, 0x33, 0x10, 0x42, 0x14 };
    // tela "Mistura": a sonda a cada 2 pedidos, o resto so o necessario
    static const uint8_t mix[] = { 0x06, 0x0C, 0x07, 0x05, 0x06, 0x0C, 0x0B, 0x0D };
    static int i = 0;
    static bool rpm_turn = true;
    uint8_t pid;
    bool focus = s_mix_focus && obd2_is_supported(&s_sup, 0x14);
    if (rpm_turn) {
        pid = focus ? 0x14 : 0x0C;
    } else {
        int guard = 0;
        if (focus) do { pid = mix[i++ % sizeof mix]; } while (!obd2_is_supported(&s_sup, pid) && ++guard < 20);
        else do { pid = slow[i++ % sizeof slow]; } while (!obd2_is_supported(&s_sup, pid) && ++guard < 20);
    }
    rpm_turn = !rpm_turn;
    char cmd[8];
    snprintf(cmd, sizeof cmd, "01%02X", pid);
    uint8_t r[32];
    size_t n;
    elm_status_t st = req(cmd, r, sizeof r, &n, 1000);
    if (st == ELM_OK && obd2_decode_mode01(r, n, s)) {
        s->source = SRC_OBD2;
        publish(s);
        count_rate();
        s_fail = 0;
    } else {
        s_fail++;
    }
}

// ---------------------------------------------------------------- DTC
static void read_dtcs(bool clear)
{
    uint8_t r[128];
    size_t n = 0;
    bool switched = false;
    elm_status_t st = elm_request(clear ? "04" : "03", r, sizeof r, &n, 4000);
    if (st != ELM_OK && s_mode == M_RENAULT) {
        // a ECU nao aceitou o servico OBD no canal Renault: abre canal OBD2 so pra isso
        if (connect_obd2()) {
            switched = true;
            st = elm_request(clear ? "04" : "03", r, sizeof r, &n, 4000);
        }
    }
    app_lock();
    if (st != ELM_OK) {
        snprintf(g_obd.dtc_msg, sizeof g_obd.dtc_msg, "Falha: %s", elm_status_str(st));
        g_obd.dtc_valid = false;
    } else if (clear) {
        snprintf(g_obd.dtc_msg, sizeof g_obd.dtc_msg, (r[0] == 0x44) ? "Codigos apagados" : "Resposta inesperada");
        g_obd.ndtc = 0;
        g_obd.dtc_valid = true;
    } else {
        g_obd.ndtc = obd2_decode_dtcs(r, n, 0x43, g_obd.dtcs, MAX_DTCS);
        g_obd.dtc_valid = true;
        snprintf(g_obd.dtc_msg, sizeof g_obd.dtc_msg, "%d codigo(s)", g_obd.ndtc);
    }
    g_obd.dtc_seq++;
    app_unlock();
    if (switched) {
        s_mode = M_NONE;   // reconecta no modo Renault no proximo ciclo
    }
}

// ---------------------------------------------------------------- simulador
static void demo_step(vehicle_sample_t *s, float t)
{
    const float *ratio = GEAR_JB3_RPM_PER_KMH;
    static int g = 1;                  // marcha engatada pelo "motorista" simulado
    bool sport = g_cfg.shift_mode == SHIFT_SPORT;
    float c = fmodf(t, 120.0f);
    float v, tps;
    if (c < 5) { v = 0; tps = 0; }
    // Eco: meio pedal (carga < 80 %, senao o conselho de subir nao aparece).
    // Sport: pe quase no fundo, mas < 90 % para nao "aprender" torque falso.
    else if (c < 40) { v = (c - 5) * 2.6f; tps = sport ? 85 : 45; }
    else if (c < 75) { v = 91 + 4 * sinf(c); tps = 18; }
    else if (c < 85) { v = 91 - (c - 75) * 6; tps = 0; }
    else if (c < 100) { v = 31 + 10 * sinf(c * 0.3f); tps = 25; }
    else if (c < 108) { v = fmaxf(0, 31 - (c - 100) * 4); tps = 0; }
    else { v = 0; tps = 0; }
    bool off = c >= 112;   // motor desliga no fim do ciclo -> relatorio
    // O motorista simulado troca tarde de proposito, para o indicador aparecer:
    // sobe so a 3600 rpm (6400 no modo Sport) e, no trecho de 85 a 100 s, fica
    // com preguica de reduzir e deixa o giro cair (aviso de reduzir).
    float up_at = sport ? 6400.0f : 3600.0f;
    bool lazy = c >= 85 && c < 100;
    if (v < 3) g = 1;
    else if (!lazy) {
        while (g < GEAR_JB3_N && v * ratio[g - 1] > up_at) g++;
        while (g > 1 && v * ratio[g - 1] < 1100.0f) g--;
    }
    float rpm = off ? 0 : fmaxf(850.0f, v * ratio[g - 1]);
    if (rpm > 6500) rpm = 6500;
    float load = off ? 0 : 0.25f + tps / 100.0f * 0.7f;
    s->rpm = rpm;
    s->speed_kmh = v;
    s->tps_pct = tps;
    s->map_kpa = off ? 95 : 28 + load * 67;
    s->baro_kpa = 95;
    s->coolant_c = fminf(91.0f, 35.0f + c * 1.2f);   // cada ciclo comeca "frio"
    s->iat_c = 32;
    s->vbat = off ? 12.3f : 14.1f;
    s->inj_ms = (off || (tps < 1 && rpm > 1500)) ? 0 : 2.2f + load * 5.0f;
    s->fuel_lph_ecu = s->inj_ms * rpm * 4 / 2 * 60 * (160.0f / 60000.0f) / 1000.0f;
    s->torque_nm = off ? 0 : 40 + load * 110;
    // Mistura simulada: a frio (< 60 C) a sonda fica baixa (pobre) e sem correcao;
    // quente, ela oscila ~1x/s e a correcao curta acompanha. No corte de injecao
    // (desaceleracao) cai a quase zero. So para testar a tela, nao e um modelo real.
    bool warm = s->coolant_c > 60.0f;
    float osc = sinf(t * 6.28f);
    if (off) { s->o2_v = VNAN; s->stft_pct = VNAN; }
    else if (s->inj_ms == 0) { s->o2_v = 0.03f; s->stft_pct = 0; }
    else if (!warm) { s->o2_v = 0.12f + 0.04f * osc; s->stft_pct = 0; }
    else { s->o2_v = osc > 0 ? 0.78f : 0.12f; s->stft_pct = -4.0f * osc; }
    s->ltft_pct = 6.0f;
    // A/C pedido de 3 s a 100 s; o compressor e cortado nas arrancadas (pe fundo, ou os
    // primeiros segundos de qualquer arrancada), como a ECU faria. Ventoinha: 1a vel. quente, 2a vel. de 60 a 70 s.
    bool ac_req = !off && c > 3 && c < 100;
    bool launch = (c > 5 && c < 14) || tps >= 80;
    s->ac_req = off ? VNAN : (ac_req ? 1.0f : 0.0f);
    s->ac_cmd = off ? VNAN : ((ac_req && !launch) ? 1.0f : 0.0f);
    s->fan1 = off ? VNAN : ((s->coolant_c > 85.0f && c < 100) ? 1.0f : 0.0f);
    s->fan2 = off ? VNAN : ((c > 60 && c < 70) ? 1.0f : 0.0f);
    // Direcao simulada: o volante e girado de 20 a 23 s e de 50 a 54 s do ciclo; o pressostato
    // muda e a valvula de marcha lenta abre um pouco mais. So para testar a tela.
    bool turning = (c > 20 && c < 23) || (c > 50 && c < 54);
    s->ps_conn = off ? VNAN : 1.0f;
    s->ps_level = off ? VNAN : (turning ? 1.0f : 0.0f);
    s->idle_active = off ? VNAN : ((tps < 1 && v < 6) ? 1.0f : 0.0f);
    s->idle_acc = off ? VNAN : 0.0f;
    s->idle_rco = off ? VNAN : 22.0f + (turning ? 9.0f : 0.0f);
    s->idle_set = off ? VNAN : 850.0f;
    s->o2_down_v = off ? VNAN : 0.62f;
    s->loop_closed = off ? VNAN : ((warm && s->inj_ms > 0) ? 1.0f : 0.0f);
    s->spark_adv = off ? VNAN : 10.0f + load * 12.0f;
    s->source = SRC_DEMO;
}

// ---------------------------------------------------------------- tarefa
static void obd_task(void *arg)
{
    vehicle_sample_t s;
    vehicle_sample_clear(&s);
    bool elm_ok = false;
    int64_t t0 = esp_timer_get_time();
    s_rate_t0 = t0;

    elm_init(UART_NUM_1, CONFIG_PAINEL_ELM_UART_TX, CONFIG_PAINEL_ELM_UART_RX, CONFIG_PAINEL_ELM_BAUD);

    for (;;) {
        if (g_cfg.demo) {
            // Tempo simulado acelerado (2x; 3x no Sport) e 50 amostras/s, para o
            // ponteiro andar liso. No carro o ritmo e o do ELM327, bem mais lento.
            static float demo_t = 0;
            static int demo_n = 0;
            demo_t += 0.02f * (g_cfg.shift_mode == SHIFT_SPORT ? 3.0f : 2.0f);
            demo_step(&s, demo_t);
            publish(&s);
            if (demo_n++ % 250 == 0) set_status("MODO DEMONSTRACAO (simulado)");
            obd_cmd_t c;
            while (obd_get_cmd(&c)) {}
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        obd_cmd_t c;
        while (obd_get_cmd(&c)) {
            if (c == CMD_RECONNECT) { s_mode = M_NONE; elm_ok = false; }
            else if (s_mode != M_NONE) read_dtcs(c == CMD_CLEAR_DTC);
        }

        if (!elm_ok) {
            set_status("Procurando ELM327...");
            elm_ok = elm_setup();
            if (!elm_ok) { set_offline(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        }

        if (s_mode == M_NONE) {
            vehicle_sample_clear(&s);
            bool ok = false;
            if (g_cfg.proto != PROTO_OBD2 && connect_renault()) { s_mode = M_RENAULT; ok = true; }
            else if (g_cfg.proto != PROTO_RENAULT && connect_obd2()) { s_mode = M_OBD2; ok = true; }
            if (!ok) {
                set_offline();
                set_status("ECU nao responde (chave ligada?)");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            s_fail = 0;
        }

        if (s_mode == M_RENAULT) poll_renault(&s);
        else poll_obd2(&s);

        if (s_fail >= 6) {        // ~6 pedidos seguidos sem resposta: chave desligada ou linha caiu
            set_offline();
            set_status("Conexao perdida, reconectando...");
            s_mode = M_NONE;
            elm_ok = elm_at("ATI", 500);
        }
        vTaskDelay(1);   // o tempo de verdade e o da linha K (~50-120 ms por pedido)
    }
}

void obd_task_start(void)
{
    xTaskCreatePinnedToCore(obd_task, "obd", 6144, NULL, 5, NULL, 0);
}
