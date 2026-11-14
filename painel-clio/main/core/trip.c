#include "trip.h"
#include <math.h>
#include <string.h>

#define TRIP_MAGIC 0x54524950u

void trip_init(trip_state_t *t)
{
    memset(t, 0, sizeof(*t));
    t->p.magic = TRIP_MAGIC;
}

static void acc_add(trip_accum_t *a, double dkm, double dl, double dt, int moving)
{
    a->dist_km += dkm;
    a->fuel_l += dl;
    a->time_s += dt;
    if (moving) a->moving_s += dt;
}

void trip_update(trip_state_t *t, float speed_kmh, float lph, float dt_s)
{
    if (!t->engine_running) return;
    if (dt_s <= 0 || dt_s > 5.0f) return;          // lacuna grande = dado perdido, nao inventa
    double v = (isnan(speed_kmh) || speed_kmh < 0) ? 0 : speed_kmh;
    double f = (isnan(lph) || lph < 0) ? 0 : lph;
    double dkm = v * dt_s / 3600.0;
    double dl = f * dt_s / 3600.0;
    int moving = v > 3.0;
    acc_add(&t->current, dkm, dl, dt_s, moving);
    acc_add(&t->p.since_reset, dkm, dl, dt_s, moving);
    acc_add(&t->p.since_fill, dkm, dl, dt_s, moving);
    t->p.total_dist_km += dkm;
    t->p.total_fuel_l += dl;
}

void trip_start(trip_state_t *t)
{
    memset(&t->current, 0, sizeof(t->current));
    t->engine_running = true;
}

void trip_end(trip_state_t *t)
{
    t->engine_running = false;
    t->p.last_trip = t->current;
}

void trip_reset_average(trip_state_t *t) { memset(&t->p.since_reset, 0, sizeof(t->p.since_reset)); }
void trip_fill_tank(trip_state_t *t) { memset(&t->p.since_fill, 0, sizeof(t->p.since_fill)); }

double trip_kml(const trip_accum_t *a)
{
    if (a->dist_km < 0.2 || a->fuel_l < 0.01) return NAN;
    return a->dist_km / a->fuel_l;
}

double trip_avg_speed(const trip_accum_t *a)
{
    if (a->time_s < 1) return NAN;
    return a->dist_km / (a->time_s / 3600.0);
}

double trip_range_km(const trip_state_t *t, double tank_l, double fallback_kml, double *fuel_left_l)
{
    double left = tank_l - t->p.since_fill.fuel_l;
    if (left < 0) left = 0;
    if (fuel_left_l) *fuel_left_l = left;
    double kml = NAN;
    if (t->p.since_fill.dist_km >= 30) kml = trip_kml(&t->p.since_fill);
    if (isnan(kml) && t->p.since_reset.dist_km >= 30) kml = trip_kml(&t->p.since_reset);
    if (isnan(kml)) kml = fallback_kml;
    return left * kml;
}

double trip_calibrate_factor(const trip_state_t *t, double real_l, double current_factor)
{
    double computed = t->p.since_fill.fuel_l;
    if (computed < 5.0 || real_l < 5.0) return current_factor;   // pouca amostra
    double f = current_factor * real_l / computed;
    if (f < 0.5) f = 0.5;
    if (f > 2.0) f = 2.0;
    return f;
}
