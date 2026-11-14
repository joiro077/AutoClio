#include "ecu_profiles.h"
#include <stdio.h>
#include <string.h>

static float *field_ptr(vehicle_sample_t *s, ecu_field_id_t id)
{
    switch (id) {
    case F_RPM:     return &s->rpm;
    case F_SPEED:   return &s->speed_kmh;
    case F_COOLANT: return &s->coolant_c;
    case F_IAT:     return &s->iat_c;
    case F_MAP:     return &s->map_kpa;
    case F_TPS:
    case F_TPS_DEG: return &s->tps_pct;
    case F_VBAT:    return &s->vbat;
    case F_INJ_MS:  return &s->inj_ms;
    case F_FUEL_LH: return &s->fuel_lph_ecu;
    case F_TORQUE:  return &s->torque_nm;
    case F_BARO:    return &s->baro_kpa;
    case F_O2_UP:   return &s->o2_v;
    case F_O2_DOWN: return &s->o2_down_v;
    case F_RICH:    return &s->rich_raw;
    case F_LOOP:    return &s->loop_closed;
    case F_ADV:     return &s->spark_adv;
    case F_KNOCK:   return &s->knock_ret;
    case F_AC_REQ:  return &s->ac_req;
    case F_AC_CMD:  return &s->ac_cmd;
    case F_FAN1:    return &s->fan1;
    case F_FAN2:    return &s->fan2;
    case F_PS_LEVEL:   return &s->ps_level;
    case F_PS_CONN:    return &s->ps_conn;
    case F_IDLE_ACTIVE: return &s->idle_active;
    case F_IDLE_ACC:   return &s->idle_acc;
    case F_IDLE_RCO:   return &s->idle_rco;
    case F_IDLE_SET:   return &s->idle_set;
    default:        return NULL;
    }
}

int kwp_decode_frame(const ecu_profile_t *p, const uint8_t *r, size_t len, vehicle_sample_t *out)
{
    if (!p || len < 3 || r[0] != 0x61) return 0;
    uint8_t lid = r[1];
    int n = 0;
    for (size_t i = 0; i < p->nfields; i++) {
        const ecu_field_t *f = &p->fields[i];
        if (f->lid != lid) continue;
        size_t idx = (size_t)f->firstbyte - 1;      // 1-based -> 0-based
        if (f->bits == 1) {                          // flag: bit 0 do ddt4all = bit mais significativo
            float *flag = field_ptr(out, f->id);
            if (idx < len && flag) { *flag = (float)((r[idx] >> (7 - f->bitoff)) & 1); n++; }
            continue;
        }
        size_t nb = f->bits / 8;
        if (idx + nb > len) continue;
        int32_t raw = (nb == 2) ? ((r[idx] << 8) | r[idx + 1]) : r[idx];
        if (f->is_signed) raw = (nb == 2) ? (int16_t)raw : (int8_t)raw;
        float v = ((float)raw * f->step + f->offset) / (f->div != 0 ? f->div : 1.0f);
        switch (f->id) {
        case F_MAP:
        case F_BARO:    v /= 10.0f; break;            // mbar -> kPa
        case F_O2_UP:
        case F_O2_DOWN: v /= 1000.0f; break;          // mV -> V
        case F_TPS_DEG: v = v / 85.0f * 100.0f;       // graus -> % (aprox., ~85 graus = aberta)
                        if (v > 100) v = 100;
                        break;
        default: break;
        }
        float *dst = field_ptr(out, f->id);
        if (dst) { *dst = v; n++; }
    }
    return n;
}

bool kwp_parse_ident(const uint8_t *r, size_t len, ecu_id_info_t *o)
{
    // Layout do 21 80 nas definicoes do ddt4all destas ECUs:
    // b8 = n. diag, b9..11 = fornecedor (ASCII), b17..18 = programa ("soft"), b19..20 = versao
    if (len < 20 || r[0] != 0x61 || r[1] != 0x80) return false;
    memset(o, 0, sizeof(*o));
    o->diag = r[7];
    for (int i = 0; i < 3; i++) {
        char c = (char)r[8 + i];
        o->supplier[i] = (c >= 32 && c < 127) ? c : '?';
    }
    snprintf(o->soft, sizeof(o->soft), "%02X%02X", r[16], r[17]);
    snprintf(o->version, sizeof(o->version), "%02X%02X", r[18], r[19]);
    return true;
}

int kwp_match_profile(const ecu_id_info_t *id)
{
    int best = -1, best_score = 0;
    for (int i = 0; i < g_ecu_profile_count; i++) {
        for (const ecu_ident_t *e = g_ecu_profiles[i].ids; e && e->soft; e++) {
            int score = 0;
            if (strcmp(e->soft, id->soft) == 0) score += 2; else continue;
            if (strcmp(e->version, id->version) == 0) score += 2;
            if (strcmp(e->supplier, id->supplier) == 0) score += 1;
            if (score > best_score) { best_score = score; best = i; }
        }
    }
    return best;
}

int kwp_rpm_lid(const ecu_profile_t *p)
{
    for (size_t i = 0; i < p->nfields; i++)
        if (p->fields[i].id == F_RPM) return p->fields[i].lid;
    return -1;
}
