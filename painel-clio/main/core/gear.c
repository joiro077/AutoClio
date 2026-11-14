#include "gear.h"
#include <math.h>

// JB3 do Clio 1.6 16V. SUPOSICAO (valores de catalogo citados de memoria, nao
// medidos no carro): 1a 3,364  2a 1,864  3a 1,321  4a 1,029  5a 0,795, diferencial
// 4,067 (15/61), pneu 175/65 R14 com ~1,80 m de circunferencia de rodagem.
// rpm/kmh = relacao * diferencial * 1000 / (60 * 1,80)
const float GEAR_JB3_RPM_PER_KMH[GEAR_JB3_N] = { 126.7f, 70.2f, 49.7f, 38.7f, 29.9f };

int gear_detect(const settings_t *c, float rpm, float v)
{
    if (!VALID(rpm) || !VALID(v) || v < 5.0f || rpm < 650.0f) return 0;
    float ratio = rpm / v;
    int best = 0;
    float best_err = 1e9f;
    for (int g = 0; g < c->ngears && g < MAX_GEARS; g++) {
        float r = c->gear_ratio[g];
        if (r <= 0) continue;
        float err = fabsf(ratio - r) / r;
        if (err < best_err) { best_err = err; best = g + 1; }
    }
    return (best_err <= c->gear_tol) ? best : 0;   // fora da tolerancia: embreagem/neutro
}

void gear_learn_add(gear_learn_acc_t *a, float rpm, float v)
{
    if (!VALID(rpm) || !VALID(v) || v < 15.0f || rpm < 1000.0f) return;
    a->sum += rpm / v;
    a->n++;
}

bool gear_learn_commit(settings_t *c, int g, const gear_learn_acc_t *a)
{
    if (g < 1 || g > MAX_GEARS || a->n < 10) return false;
    c->gear_ratio[g - 1] = (float)(a->sum / a->n);
    if (g > c->ngears) c->ngears = (uint8_t)g;
    return true;
}

float engine_load01(const vehicle_sample_t *s)
{
    if (VALID(s->load_pct)) return s->load_pct / 100.0f;
    if (VALID(s->map_kpa)) {
        float baro = VALID(s->baro_kpa) ? s->baro_kpa : 100.0f;
        float l = s->map_kpa / baro;
        return l > 1.0f ? 1.0f : l;
    }
    if (VALID(s->tps_pct)) return s->tps_pct / 100.0f;
    return VNAN;
}

static float tq_at(const settings_t *c, float rpm)
{
    float x = (rpm - 1000.0f) / 500.0f;
    if (x < 0 || x > TQ_BINS - 1) return VNAN;
    int i = (int)x;
    if (i >= TQ_BINS - 1) return c->tq_curve[TQ_BINS - 1] > 0 ? c->tq_curve[TQ_BINS - 1] : VNAN;
    float a = c->tq_curve[i], b = c->tq_curve[i + 1];
    if (a <= 0 || b <= 0) return VNAN;
    float f = x - i;
    return a + (b - a) * f;
}

float shift_optimal_upshift_rpm(const settings_t *c, int g)
{
    if (g < 1 || g >= c->ngears) return VNAN;
    float rg = c->gear_ratio[g - 1], rn = c->gear_ratio[g];
    if (rg <= 0 || rn <= 0) return VNAN;
    float k = rn / rg;   // rpm cai por este fator na troca
    // Forca na roda ~ T(rpm) * relacao. Vale subir quando a forca na marcha de
    // cima (no rpm que vai cair) fica >= a da marcha atual.
    // Isso NAO e o pico de torque: normalmente fica perto/depois do pico de potencia.
    for (float rpm = 3000.0f; rpm <= c->redline_rpm; rpm += 50.0f) {
        float t_now = tq_at(c, rpm), t_next = tq_at(c, rpm * k);
        if (!VALID(t_now) || !VALID(t_next)) return VNAN;
        if (t_now <= t_next * k) return rpm;
    }
    return c->redline_rpm - 200.0f;   // nunca cruza: troca perto do corte
}

void torque_learn(settings_t *c, const vehicle_sample_t *s)
{
    if (!VALID(s->torque_nm) || !VALID(s->rpm) || s->torque_nm <= 0) return;
    float load = engine_load01(s);
    bool wot = (VALID(s->tps_pct) && s->tps_pct > 90.0f) || (VALID(load) && load > 0.95f);
    if (!wot) return;
    float x = (s->rpm - 1000.0f) / 500.0f;
    int i = (int)lroundf(x);
    if (i < 0 || i >= TQ_BINS || fabsf(x - i) > 0.25f) return;
    float *b = &c->tq_curve[i];
    *b = (*b <= 0) ? s->torque_nm : (*b * 0.8f + s->torque_nm * 0.2f);
}

shift_advice_t shift_update(shift_state_t *st, const settings_t *c, const vehicle_sample_t *s,
                            int gear, float dt_s)
{
    if (dt_s > 0 && VALID(s->rpm)) {
        float rate = (s->rpm - st->last_rpm) / dt_s;
        if (fabsf(rate) < 20000.0f) st->rpm_rate = st->rpm_rate * 0.7f + rate * 0.3f;
        st->last_rpm = s->rpm;
    }
    if (st->hold_s > 0) { st->hold_s -= dt_s; return st->advice; }

    shift_advice_t adv = SHIFT_NONE;
    float load = engine_load01(s);
    if (c->shift_mode == SHIFT_OFF || gear == 0 || !VALID(s->rpm) || !VALID(load)) {
        st->advice = SHIFT_NONE;
        return SHIFT_NONE;
    }
    float rpm = s->rpm;
    float r_cur = c->gear_ratio[gear - 1];
    float rpm_up = (gear < c->ngears && c->gear_ratio[gear] > 0) ? rpm * c->gear_ratio[gear] / r_cur : VNAN;
    float rpm_dn = (gear > 1 && c->gear_ratio[gear - 2] > 0) ? rpm * c->gear_ratio[gear - 2] / r_cur : VNAN;

    if (c->shift_mode == SHIFT_ECO) {
        // "arregando": carga alta, rpm baixo e caindo (sua ideia da subida)
        bool lugging = load >= c->lug_load && rpm < c->lug_rpm && st->rpm_rate < -30.0f;
        st->lug_s = lugging ? st->lug_s + dt_s : 0;
        if (VALID(rpm_dn) && rpm_dn < c->redline_rpm - 800.0f &&
            (rpm < c->eco_down_rpm || st->lug_s > 1.5f)) {
            adv = SHIFT_DOWN;
        } else if (VALID(rpm_up) && rpm >= c->eco_up_rpm && rpm_up >= c->eco_min_after_rpm && load < 0.80f) {
            adv = SHIFT_UP;
        }
    } else { // SPORT
        float up = shift_optimal_upshift_rpm(c, gear);
        if (!VALID(up)) up = c->sport_up_rpm;
        if (VALID(rpm_up) && rpm >= up) {
            adv = SHIFT_UP;
        } else if (VALID(rpm_dn) && load > 0.85f && rpm < c->sport_down_rpm && rpm_dn < up - 300.0f) {
            adv = SHIFT_DOWN;   // pe embaixo e fora da faixa forte: reduz se couber
        }
    }
    if (adv != st->advice && adv != SHIFT_NONE) st->hold_s = 1.0f;
    st->advice = adv;
    return adv;
}
