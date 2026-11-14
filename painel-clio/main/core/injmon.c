#include "injmon.h"
#include "gear.h"
#include <math.h>
#include <string.h>

#define MIN_SAMPLES   60
#define DEV_LIMIT_PCT 20.0f
#define ALERT_AFTER_S 5.0f
#define TRIM_LIMIT    15.0f

void injmon_init(injmon_t *m) { memset(m, 0, sizeof(*m)); }

injmon_status_t injmon_update(injmon_t *m, const vehicle_sample_t *s, float dt_s)
{
    // Correcoes de combustivel (so existem no modo OBD2)
    if (VALID(s->ltft_pct)) {
        float total = s->ltft_pct + (VALID(s->stft_pct) ? s->stft_pct : 0);
        if (fabsf(s->ltft_pct) > TRIM_LIMIT || fabsf(total) > TRIM_LIMIT + 7.0f) {
            m->status = TRIM_HIGH;
            return m->status;
        }
    }
    if (!VALID(s->inj_ms) || !VALID(s->rpm)) { m->status = INJ_OK; return m->status; }
    // so motor quente e em regime: frio/partida enriquecem de proposito
    if (!VALID(s->coolant_c) || s->coolant_c < 80.0f || s->rpm < 700.0f || s->inj_ms < 0.5f) {
        m->alert_s = 0;
        return m->status = INJ_OK;
    }
    float load = engine_load01(s);
    if (!VALID(load) || load < 0.2f) return m->status;   // desaceleracao/corte
    int ri = (int)((s->rpm - 800.0f) / 700.0f);
    int li = (int)((load - 0.2f) / 0.1f);
    if (ri < 0 || ri >= INJ_RPM_BINS || li < 0 || li >= INJ_LOAD_BINS) return m->status;

    float *b = &m->map.base[ri][li];
    uint16_t *n = &m->map.n[ri][li];
    if (*n < MIN_SAMPLES) {
        *b = (*n == 0) ? s->inj_ms : (*b * *n + s->inj_ms) / (*n + 1);
        (*n)++;
        m->dev_pct = 0;
        return m->status = INJ_LEARNING;
    }
    m->dev_pct = (s->inj_ms - *b) / *b * 100.0f;
    if (fabsf(m->dev_pct) > DEV_LIMIT_PCT) {
        m->alert_s += dt_s;
    } else {
        m->alert_s = 0;
        *b = *b * 0.995f + s->inj_ms * 0.005f;   // acompanha mudancas lentas (clima, gasolina)
    }
    if (m->alert_s > ALERT_AFTER_S) m->status = m->dev_pct > 0 ? INJ_HIGH : INJ_LOW;
    else m->status = INJ_OK;
    return m->status;
}

const char *injmon_str(injmon_status_t st)
{
    switch (st) {
    case INJ_LEARNING: return "aprendendo";
    case INJ_HIGH: return "INJECAO ALTA";
    case INJ_LOW: return "INJECAO BAIXA";
    case TRIM_HIGH: return "CORRECAO ALTA";
    default: return "ok";
    }
}
