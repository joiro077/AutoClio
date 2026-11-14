// injmon.h - monitor de tempo de injecao e correcoes de combustivel
//
// Ideia: o tempo de injecao num mesmo ponto de operacao (rpm x carga) deveria
// ser repetivel com o motor quente. O monitor aprende uma "linha de base" por
// celula e avisa quando o valor atual fica muito longe dela por varios segundos.
// Isso indica TENDENCIA (bico sujo, entrada falsa de ar, sonda, pressao de
// combustivel...), nao diagnostica a causa. Com OBD2, STFT/LTFT sao o indicador
// mais direto e sao checados tambem.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "vehicle.h"

#define INJ_RPM_BINS  8   // 800..6400 rpm
#define INJ_LOAD_BINS 8   // carga 0.2..1.0

typedef struct {
    float base[INJ_RPM_BINS][INJ_LOAD_BINS];
    uint16_t n[INJ_RPM_BINS][INJ_LOAD_BINS];
} injmon_map_t;   // vai para a NVS

typedef enum {
    INJ_OK = 0,
    INJ_LEARNING,
    INJ_HIGH,      // injetando bem mais que o normal (mistura pobre sendo corrigida?)
    INJ_LOW,
    TRIM_HIGH,     // LTFT/STFT altos
} injmon_status_t;

typedef struct {
    injmon_map_t map;
    float dev_pct;     // desvio atual em relacao a base
    float alert_s;
    injmon_status_t status;
} injmon_t;

void injmon_init(injmon_t *m);
injmon_status_t injmon_update(injmon_t *m, const vehicle_sample_t *s, float dt_s);
const char *injmon_str(injmon_status_t st);
