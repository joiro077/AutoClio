// app_state.h - estado compartilhado entre as tarefas (OBD, logica, tela)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ecu_profiles.h"
#include "fuel.h"
#include "gear.h"
#include "injmon.h"
#include "settings.h"
#include "trip.h"
#include "vehicle.h"

#define MAX_DTCS 24

// Escrito pela tarefa OBD
typedef struct {
    vehicle_sample_t s;
    char status[64];          // "Conectado (Renault KWP)", "ELM nao responde", ...
    char ecu_name[48];
    ecu_id_info_t ecu_id;
    float req_per_s;
    char dtcs[MAX_DTCS][6];
    int ndtc;
    bool dtc_valid;
    uint32_t dtc_seq;         // muda a cada leitura
    char dtc_msg[64];
} obd_shared_t;

// Escrito pela tarefa de logica, lido pela tela
typedef struct {
    vehicle_sample_t s;
    float lph;
    fuel_src_t fuel_src;
    float inst;               // km/L ou L/100
    double avg_trip, avg_reset;   // km/L
    double range_km, fuel_left_l;
    trip_accum_t current;
    trip_accum_t last_trip;
    bool engine_running;
    bool show_report;         // abrir o relatorio de viagem
    uint32_t report_seq;
    int gear;
    shift_advice_t shift;
    injmon_status_t inj_status;
    float inj_dev_pct;
    float sport_up_rpm;       // rpm de troca em uso no modo sport (NAN = fixo)
    bool learning_gear;
    int learn_gear_n;
    int learn_samples;
} dash_t;

typedef enum {
    CMD_READ_DTC = 1,
    CMD_CLEAR_DTC,
    CMD_RECONNECT,
} obd_cmd_t;

typedef enum {
    LCMD_RESET_AVG = 1,
    LCMD_FILL_TANK,
    LCMD_CALIBRATE,       // arg = litros reais abastecidos
    LCMD_LEARN_GEAR,      // arg = numero da marcha
    LCMD_SAVE_SETTINGS,
    LCMD_REPORT_SEEN,
    LCMD_CLEAR_TQ,
} logic_cmd_t;

typedef struct { logic_cmd_t cmd; float arg; } logic_msg_t;

extern obd_shared_t g_obd;
extern dash_t g_dash;
extern settings_t g_cfg;      // protegida pelo mesmo mutex

// Registro de eventos do A/C (para ver se a injecao corta o compressor nas arrancadas).
// Fica so na RAM (perde ao desligar) e tambem sai na serial. Protegido pelo mesmo mutex.
#define MAX_EVENTS 24
typedef enum {
    EV_AC_REQ_ON = 0, EV_AC_REQ_OFF, EV_AC_ON, EV_AC_OFF, EV_AC_CUT,
    EV_PS_HI, EV_PS_LO,      // pressostato da direcao mudou para ATIVO (nivel 1) / inativo (nivel 0)
    EV_RPM_DROP,             // rotacao caiu de >= 650 para < 450 (motor morrendo, ou chave desligada)
    EV_TYPES
} ev_type_t;
typedef struct {
    uint32_t t_s;        // segundos desde que o painel ligou
    uint8_t type;        // ev_type_t
    uint8_t tps;         // borboleta %
    uint16_t rpm;
    uint8_t speed;       // km/h
    uint8_t rco;         // valvula de marcha lenta %, 255 = a ECU nao informa
} ev_t;
extern const char *const g_ev_names[EV_TYPES];
typedef struct {
    ev_t e[MAX_EVENTS];  // anel: o mais novo esta em (head - 1)
    int n;               // quantos validos (ate MAX_EVENTS)
    int head;
    uint32_t seq;        // sobe a cada evento novo
} ev_log_t;
extern ev_log_t g_ev;

void app_state_init(void);
void app_lock(void);
void app_unlock(void);

void obd_post_cmd(obd_cmd_t c);
bool obd_get_cmd(obd_cmd_t *c);   // nao bloqueia
void logic_post(logic_cmd_t c, float arg);
bool logic_get(logic_msg_t *m);   // nao bloqueia

void logic_task_start(void);
