#include "fuel.h"

#define R_AIR 287.05f   // J/(kg.K)

float fuel_lph(const vehicle_sample_t *s, const settings_t *c, fuel_src_t *src)
{
    fuel_src_t used = FUEL_SRC_NONE;
    float lph = VNAN;

    if (!VALID(s->rpm) || s->rpm < 200.0f) {   // motor parado
        if (src) *src = FUEL_SRC_NONE;
        return 0.0f;
    }

    if (VALID(s->fuel_lph_ecu)) {
        lph = s->fuel_lph_ecu;
        used = FUEL_SRC_ECU;
    } else if (VALID(s->inj_ms)) {
        // injecao sequencial, 4 tempos: cada cilindro injeta 1 vez a cada 2 voltas
        float pulse_ms = s->inj_ms - c->inj_dead_ms;
        if (pulse_ms < 0) pulse_ms = 0;
        float inj_per_h = (s->rpm / 2.0f) * 60.0f * c->ncyl;
        float cc_per_ms = c->inj_flow_ccmin / 60000.0f;
        lph = inj_per_h * pulse_ms * cc_per_ms / 1000.0f;
        used = FUEL_SRC_INJ;
    } else if (VALID(s->maf_gs)) {
        float fuel_gs = s->maf_gs / c->afr;
        lph = fuel_gs * 3.6f / c->fuel_density;
        used = FUEL_SRC_MAF;
    } else if (VALID(s->map_kpa)) {
        // corte de combustivel na desaceleracao: borboleta fechada e rpm alto
        if (VALID(s->tps_pct) && s->tps_pct < 1.5f && s->rpm > 1700.0f) {
            lph = 0.0f;
        } else {
            float iat_k = (VALID(s->iat_c) ? s->iat_c : 30.0f) + 273.15f;
            float rho = (s->map_kpa * 1000.0f) / (R_AIR * iat_k);                // kg/m3
            float vol_m3s = (c->displacement_l / 1000.0f) * c->ve * s->rpm / 120.0f;
            float air_kgs = rho * vol_m3s;
            float fuel_kgs = air_kgs / c->afr;
            lph = fuel_kgs * 3600.0f / c->fuel_density;
        }
        used = FUEL_SRC_MAP;
    }

    if (src) *src = used;
    if (!VALID(lph)) return VNAN;
    return lph * c->fuel_cal;
}

float fuel_instant_display(float lph, float speed_kmh, int units_kml)
{
    if (!VALID(lph) || !VALID(speed_kmh) || speed_kmh < 3.0f) return VNAN;
    if (units_kml) {
        if (lph < 0.05f) return 99.9f;           // corte de combustivel (banguela engrenada)
        float v = speed_kmh / lph;
        return v > 99.9f ? 99.9f : v;
    }
    return lph * 100.0f / speed_kmh;             // L/100km
}

const char *fuel_src_str(fuel_src_t s)
{
    switch (s) {
    case FUEL_SRC_ECU: return "ECU";
    case FUEL_SRC_INJ: return "T.INJ";
    case FUEL_SRC_MAF: return "MAF";
    case FUEL_SRC_MAP: return "MAP";
    default: return "--";
    }
}
