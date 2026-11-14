// vehicle.h - tipos compartilhados (C puro, sem dependencia do ESP-IDF,
// para poder ser testado no PC com gcc).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#define VNAN NAN
#define VALID(x) (!isnan(x))

typedef enum {
    SRC_NONE = 0,
    SRC_RENAULT,   // KWP2000 Renault 21xx (mesmos dados do ddt4all)
    SRC_OBD2,      // SAE J1979 padrao (mesmos dados do Car Scanner "OBD-II/EOBD")
    SRC_DEMO,      // simulador, para testar a tela na bancada
} data_source_t;

// Uma "foto" do motor. Campo que a ECU nao fornece fica NAN.
typedef struct {
    float rpm;
    float speed_kmh;
    float coolant_c;
    float iat_c;
    float map_kpa;        // pressao no coletor
    float baro_kpa;       // pressao atmosferica
    float tps_pct;        // borboleta 0..100 %
    float load_pct;       // carga calculada (PID 04) ou MAP/baro
    float inj_ms;         // tempo de injecao real
    float fuel_lph_ecu;   // consumo instantaneo calculado PELA ECU (L/h)
    float torque_nm;      // torque estimado pela ECU (modelo da ECU, nao dinamometro)
    float vbat;
    float stft_pct, ltft_pct;  // correcoes de combustivel (so via OBD2)
    float o2_v;           // tensao da sonda lambda antes do catalisador (0..1 V, narrowband)
    float o2_down_v;      // sonda depois do catalisador (V)
    float rich_raw;       // fator de richesse da ECU, valor bruto (escala/neutro ainda nao confirmados)
    float loop_closed;    // 1 = regulacao de richesse ativa (malha fechada), 0 = aberta
    float spark_adv;      // avance a l'allumage (graus)
    float ac_req, ac_cmd; // A/C: pedido e comando do compressor (0/1, NAN = a ECU nao informa)
    float fan1, fan2;     // ventoinha em 1a / 2a velocidade (0/1)
    float ps_level, ps_conn;          // pressostato da direcao: nivel eletrico e "conectado" (0/1)
    float idle_active, idle_acc;      // regulacao de marcha lenta ativa / pedido de marcha lenta acelerada (0/1)
    float idle_rco, idle_set;         // valvula de marcha lenta (%) e rotacao de consigna (rpm)
    float knock_ret;     // correcao de avance por detonacao (graus)
    float maf_gs;       // so se o carro tiver MAF (estes nao tem)
    uint32_t t_ms;        // quando a amostra foi atualizada
    uint32_t seq;         // contador de atualizacoes
    bool ecu_online;
    data_source_t source;
} vehicle_sample_t;

void vehicle_sample_clear(vehicle_sample_t *s);
