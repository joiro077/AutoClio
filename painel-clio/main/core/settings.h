// settings.h - configuracoes do painel (salvas na NVS)
#pragma once
#include <stdint.h>

#define SETTINGS_MAGIC   0x434C494Fu  // "CLIO"
#define SETTINGS_VERSION 1
#define MAX_GEARS        6
#define TQ_BINS          14           // curva de torque: 1000..7500 rpm, passo 500

enum { SHIFT_OFF = 0, SHIFT_ECO = 1, SHIFT_SPORT = 2 };
enum { PROTO_AUTO = 0, PROTO_RENAULT = 1, PROTO_OBD2 = 2 };

typedef struct {
    uint32_t magic;
    uint16_t version;

    // Viagem / combustivel
    float fuel_price;      // R$/litro
    float tank_l;          // capacidade do tanque (CONFIRA no manual)
    float fuel_cal;        // fator de correcao do consumo (ajustado no abastecimento)
    float dist_cal;        // fator de correcao da distancia (velocidade OBD x hodometro/GPS)
    uint8_t units_kml;     // 1 = km/L, 0 = L/100km

    // Motor (para estimar consumo quando a ECU nao manda o valor pronto)
    float displacement_l;  // K4M = 1.598 L ; D7D = 1.149 L
    uint8_t ncyl;
    float ve;              // eficiencia volumetrica media (estimativa)
    float afr;             // estequiometrica da gasolina brasileira (E27~E30) ~13.0
    float fuel_density;    // kg/L
    float inj_flow_ccmin;  // vazao do bico (SUPOSICAO, calibrar)
    float inj_dead_ms;     // tempo morto do bico (SUPOSICAO)

    // Marchas: "rpm por km/h" de cada marcha (aprendido no menu)
    float gear_ratio[MAX_GEARS];
    uint8_t ngears;
    float gear_tol;        // tolerancia relativa (0.07 = 7 %)

    // Indicador de troca
    uint8_t shift_mode;
    float eco_up_rpm;        // sugere subir a partir daqui (eco)
    float eco_min_after_rpm; // ...desde que o rpm depois da troca fique acima disso
    float eco_down_rpm;      // abaixo disso sugere reduzir
    float lug_rpm;           // com carga alta e rpm abaixo disso = motor "arregando"
    float lug_load;          // carga (0..1) considerada alta
    float sport_up_rpm;      // usado se a curva de torque nao estiver aprendida
    float sport_down_rpm;
    float redline_rpm;

    // Curva de torque aprendida da propria ECU em pe embaixo (Nm), 0 = sem dado
    float tq_curve[TQ_BINS];

    int8_t ecu_profile;    // -1 = automatico
    uint8_t proto;
    uint8_t demo;
    uint8_t brightness;    // 0..100
} settings_t;

void settings_defaults(settings_t *s);
