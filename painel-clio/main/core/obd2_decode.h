// obd2_decode.h - SAE J1979 (o que o Car Scanner le no perfil "OBD-II / EOBD")
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vehicle.h"

// Atualiza a amostra a partir de uma resposta de modo 01 ("41 PID A B ...").
// Retorna true se o PID foi reconhecido.
bool obd2_decode_mode01(const uint8_t *r, size_t len, vehicle_sample_t *s);

// Mascara de PIDs suportados (respostas de 01 00 / 01 20 / 01 40).
typedef struct { uint32_t mask[3]; } obd2_supported_t;
void obd2_parse_supported(const uint8_t *r, size_t len, obd2_supported_t *sup);
bool obd2_is_supported(const obd2_supported_t *sup, uint8_t pid);

// Modo 03/07: DTCs. Em K-Line cada quadro e "43 A B C D E F" (3 codigos,
// 0000 = vazio) e quadros multiplos vem concatenados (43 ... 43 ...).
// Escreve codigos tipo "P0301". Retorna quantos.
int obd2_decode_dtcs(const uint8_t *r, size_t len, uint8_t mode_resp, char codes[][6], int max);
