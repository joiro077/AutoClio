#include "settings.h"
#include "vehicle.h"
#include <string.h>

void vehicle_sample_clear(vehicle_sample_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rpm = s->speed_kmh = s->coolant_c = s->iat_c = VNAN;
    s->map_kpa = s->baro_kpa = s->tps_pct = s->load_pct = VNAN;
    s->inj_ms = s->fuel_lph_ecu = s->torque_nm = s->vbat = VNAN;
    s->stft_pct = s->ltft_pct = s->maf_gs = s->o2_v = VNAN;
    s->o2_down_v = s->rich_raw = s->loop_closed = s->spark_adv = s->knock_ret = VNAN;
    s->ac_req = s->ac_cmd = s->fan1 = s->fan2 = VNAN;
    s->ps_level = s->ps_conn = s->idle_active = s->idle_acc = s->idle_rco = s->idle_set = VNAN;
}

// Valores padrao. Os marcados SUPOSICAO nao foram verificados para o seu carro:
// ajuste no menu ou deixe a calibracao corrigir.
void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->magic = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;

    s->fuel_price = 6.29f;      // exemplo, troque no menu
    s->tank_l = 50.0f;          // Clio II: 50 L (CONFIRA no manual do seu carro)
    s->fuel_cal = 1.0f;
    s->dist_cal = 1.0f;
    s->units_kml = 1;

    s->displacement_l = 1.598f; // K4M 1.6 16V
    s->ncyl = 4;
    s->ve = 0.85f;              // SUPOSICAO
    s->afr = 13.0f;             // gasolina C brasileira (com etanol), nao 14.7
    s->fuel_density = 0.75f;
    s->inj_flow_ccmin = 160.0f; // SUPOSICAO
    s->inj_dead_ms = 0.8f;      // SUPOSICAO

    s->ngears = 5;
    s->gear_tol = 0.07f;        // ratios ficam 0 ate voce aprender no menu

    s->shift_mode = SHIFT_ECO;
    s->eco_up_rpm = 2300.0f;
    s->eco_min_after_rpm = 1600.0f;
    s->eco_down_rpm = 1250.0f;
    s->lug_rpm = 1900.0f;
    s->lug_load = 0.88f;
    s->sport_up_rpm = 6000.0f;  // SUPOSICAO ate a curva de torque ser aprendida
    s->sport_down_rpm = 3500.0f;
    s->redline_rpm = 6500.0f;   // SUPOSICAO, confira o corte do seu motor

    s->ecu_profile = -1;
    s->proto = PROTO_AUTO;
    s->demo = 0;
    s->brightness = 90;
}
