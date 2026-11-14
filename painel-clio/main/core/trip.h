// trip.h - computador de bordo: integra distancia e combustivel
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double dist_km;
    double fuel_l;
    double time_s;      // tempo com motor ligado
    double moving_s;    // tempo andando (> 3 km/h)
} trip_accum_t;

// O que vai para a NVS (sobrevive ao desligar)
typedef struct {
    uint32_t magic;
    trip_accum_t since_reset;   // "media desde o ultimo reset"
    trip_accum_t since_fill;    // desde o ultimo "tanque cheio"
    trip_accum_t last_trip;     // ultima viagem completa (para o relatorio)
    double total_fuel_l;        // total geral (nunca zera, so informativo)
    double total_dist_km;
} trip_persist_t;

typedef struct {
    trip_persist_t p;
    trip_accum_t current;       // viagem atual (desde que o motor ligou)
    bool engine_running;
} trip_state_t;

void trip_init(trip_state_t *t);
// speed em km/h ja corrigida, lph em L/h, dt em s
void trip_update(trip_state_t *t, float speed_kmh, float lph, float dt_s);
void trip_start(trip_state_t *t);                 // motor ligou: zera a viagem atual
void trip_end(trip_state_t *t);                   // motor desligou: copia para last_trip
void trip_reset_average(trip_state_t *t);
void trip_fill_tank(trip_state_t *t);

// km/L de um acumulador (NAN se pouca distancia)
double trip_kml(const trip_accum_t *a);
double trip_avg_speed(const trip_accum_t *a);     // km/h sobre o tempo com motor ligado

// Autonomia em km: (tanque - gasto desde o abastecimento) * media.
// Usa a media desde o abastecimento se ja tiver >= 30 km, senao a media
// desde o reset, senao fallback_kml.
double trip_range_km(const trip_state_t *t, double tank_l, double fallback_kml, double *fuel_left_l);

// Calibracao: voce abasteceu 'real_l' litros ate completar. O painel tinha
// computado 'since_fill.fuel_l'. Retorna o novo fator (fator_atual * real/computado).
double trip_calibrate_factor(const trip_state_t *t, double real_l, double current_factor);
