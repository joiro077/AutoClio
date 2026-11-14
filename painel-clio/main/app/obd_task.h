#pragma once
#include <stdbool.h>
// Tarefa que conversa com a ECU (via ELM327) ou roda o simulador.
void obd_task_start(void);
// Tela "Mistura" aberta: no OBD2 padrao, pede a sonda e as correcoes com prioridade.
void obd_set_mix_focus(bool on);
// Tela de direcao/marcha lenta aberta: no modo Renault, le so o pressostato, a valvula de
// marcha lenta, a consigna e o rpm, para amostrar mais rapido (importa para pegar o motor morrendo).
void obd_set_steer_focus(bool on);
