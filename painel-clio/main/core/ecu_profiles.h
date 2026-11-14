// ecu_profiles.h - onde cada valor fica nas respostas 21xx de cada ECU Renault.
// A tabela (ecu_profiles_gen.c) e gerada pelo tools/ddt2profile.py a partir
// das definicoes do ddt4all.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vehicle.h"

typedef enum {
    F_RPM, F_SPEED, F_COOLANT, F_IAT, F_MAP, F_TPS, F_TPS_DEG, F_VBAT,
    F_INJ_MS, F_FUEL_LH, F_TORQUE, F_BARO,
    F_O2_UP, F_O2_DOWN,   // sonda antes / depois do catalisador (mV no DDT, V na amostra)
    F_RICH,               // fator de correcao da regulacao de richesse (valor bruto da ECU)
    F_LOOP,               // 1 bit: regulacao de richesse ativa (malha fechada)
    F_ADV,                // avance a l'allumage (graus de virabrequim)
    F_KNOCK,              // correcao maxima de avance por detonacao
    F_AC_REQ,             // 1 bit: pedido de ligar o compressor do A/C (vindo do painel do A/C)
    F_AC_CMD,             // 1 bit: a ECU esta comandando o compressor
    F_FAN1, F_FAN2,       // 1 bit: ventoinha do radiador em 1a / 2a velocidade
    F_PS_LEVEL,           // 1 bit: nivel eletrico do pressostato da direcao hidraulica
    F_PS_CONN,            // 1 bit: a ECU considera o pressostato conectado/usado
    F_IDLE_ACTIVE,        // 1 bit: regulacao de marcha lenta ativa
    F_IDLE_ACC,           // 1 bit: pedido de marcha lenta acelerada
    F_IDLE_RCO,           // abertura (duty) da valvula de marcha lenta, %
    F_IDLE_SET,           // rotacao de consigna da marcha lenta, rpm
    F__COUNT
} ecu_field_id_t;

typedef struct {
    ecu_field_id_t id;
    uint8_t lid;        // local identifier: requisicao "21 <lid>"
    uint8_t firstbyte;  // 1-based, contando o 0x61 da resposta
    uint8_t bits;       // 8 ou 16 (big endian)
    uint8_t is_signed;
    float step, offset, div;   // valor = (bruto*step + offset)/div
    uint8_t bitoff;            // so para bits == 1: posicao a partir do bit MAIS significativo (0 = bit 7)
} ecu_field_t;

typedef struct {
    const char *supplier, *soft, *version, *diag;
} ecu_ident_t;

typedef struct {
    const char *name;
    const char *file;
    bool fastinit;            // false = init a 5 baud
    uint8_t lids[6];
    uint8_t nlids;
    const ecu_field_t *fields;
    size_t nfields;
    const ecu_ident_t *ids;   // termina com {NULL,...}
} ecu_profile_t;

extern const ecu_profile_t g_ecu_profiles[];
extern const int g_ecu_profile_count;

// Decodifica uma resposta positiva "61 <lid> ..." e preenche os campos.
// Retorna quantos campos foram atualizados.
int kwp_decode_frame(const ecu_profile_t *p, const uint8_t *resp, size_t len, vehicle_sample_t *out);

// Identificacao (resposta de 21 80). Preenche strings para mostrar na tela.
typedef struct {
    char supplier[4];
    char soft[5];
    char version[5];
    int diag;
} ecu_id_info_t;
bool kwp_parse_ident(const uint8_t *resp, size_t len, ecu_id_info_t *out);

// Procura o perfil que bate com a identificacao. -1 se nenhum.
int kwp_match_profile(const ecu_id_info_t *id);

// Indice do lid que tem o RPM (para ser lido com prioridade). -1 se nenhum.
int kwp_rpm_lid(const ecu_profile_t *p);
