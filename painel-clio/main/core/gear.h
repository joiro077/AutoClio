// gear.h - marcha calculada (rpm / km/h) e indicador de troca eco/sport
#pragma once
#include <stdbool.h>
#include "settings.h"
#include "vehicle.h"

// Cambio Renault JB3 (Clio 1.6 16V), em "rpm por km/h", da 1a a 5a.
// Usado so no modo demonstracao; no carro vale o que for aprendido no menu.
#define GEAR_JB3_N 5
extern const float GEAR_JB3_RPM_PER_KMH[GEAR_JB3_N];

// 0 = neutro / embreagem / desconhecido. 1..ngears = marcha.
int gear_detect(const settings_t *c, float rpm, float speed_kmh);

// Aprende a relacao da marcha 'g' (1-based) com a media das amostras.
// Chame varias vezes com o carro estavel naquela marcha.
typedef struct { double sum; int n; } gear_learn_acc_t;
void gear_learn_add(gear_learn_acc_t *a, float rpm, float speed_kmh);
bool gear_learn_commit(settings_t *c, int g, const gear_learn_acc_t *a);

// Carga do motor 0..1: PID 04 > MAP/pressao atmosferica > borboleta
float engine_load01(const vehicle_sample_t *s);

typedef enum { SHIFT_NONE = 0, SHIFT_UP, SHIFT_DOWN } shift_advice_t;

typedef struct {
    float last_rpm;
    float rpm_rate;          // rpm/s filtrado
    shift_advice_t advice;
    float hold_s;            // segura o conselho um pouco para nao piscar
    float lug_s;             // ha quanto tempo esta arregando
} shift_state_t;

shift_advice_t shift_update(shift_state_t *st, const settings_t *c, const vehicle_sample_t *s,
                            int gear, float dt_s);

// Rpm ideal de subida em aceleracao maxima da marcha g para g+1, usando a curva
// de torque aprendida. Retorna NAN se a curva nao cobrir a faixa necessaria.
float shift_optimal_upshift_rpm(const settings_t *c, int g);

// Aprende a curva de torque usando o "couple moteur effectif" da ECU em pe embaixo.
void torque_learn(settings_t *c, const vehicle_sample_t *s);
