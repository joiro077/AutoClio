// elm_parse.h - interpreta o texto que o ELM327 devolve (C puro, testavel no PC)
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ELM_OK = 0,
    ELM_NO_DATA,        // "NO DATA": ECU nao respondeu
    ELM_UNABLE,         // "UNABLE TO CONNECT" / "BUS INIT: ...ERROR"
    ELM_UNKNOWN_CMD,    // "?"
    ELM_BUS_ERROR,      // "BUS ERROR", "FB ERROR", "DATA ERROR", "<RX ERROR"...
    ELM_STOPPED,
    ELM_NEGATIVE,       // resposta negativa do KWP (7F xx nn)
    ELM_TIMEOUT,        // sem '>' no tempo
    ELM_EMPTY,
} elm_status_t;

// Converte a resposta (com ou sem espacos, uma ou varias linhas) em bytes.
// Linhas como "SEARCHING...", "BUS INIT: ...OK" e eco do comando sao ignoradas.
// Varias linhas de dados (ex.: varios quadros do modo 03) sao concatenadas.
// Retorna o status; *n recebe o numero de bytes.
elm_status_t elm_parse_hex(const char *text, const char *echo_cmd, uint8_t *out, size_t max, size_t *n);

const char *elm_status_str(elm_status_t s);
