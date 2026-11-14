#include "elm_parse.h"
#include <ctype.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int starts(const char *line, const char *p) { return strncmp(line, p, strlen(p)) == 0; }

// remove espacos e compara com o comando enviado (eco ligado por engano)
static int is_echo(const char *line, const char *cmd)
{
    if (!cmd || !*cmd) return 0;
    const char *a = line, *b = cmd;
    while (*a && *b) {
        if (*a == ' ') { a++; continue; }
        if (*b == ' ') { b++; continue; }
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    return *a == 0 && *b == 0;
}

elm_status_t elm_parse_hex(const char *text, const char *echo_cmd, uint8_t *out, size_t max, size_t *n)
{
    *n = 0;
    char line[160];
    const char *p = text;
    int any_line = 0;
    while (*p) {
        size_t L = 0;
        while (*p && *p != '\r' && *p != '\n' && L < sizeof(line) - 1) line[L++] = *p++;
        line[L] = 0;
        while (*p == '\r' || *p == '\n') p++;
        // tira espacos e '>' nas pontas
        char *s = line;
        while (*s == ' ' || *s == '>') s++;
        size_t sl = strlen(s);
        while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '>')) s[--sl] = 0;
        if (!sl) continue;
        any_line = 1;

        if (starts(s, "SEARCHING")) continue;
        if (starts(s, "BUS INIT")) {
            if (strstr(s, "ERROR")) return ELM_UNABLE;
            continue;  // "BUS INIT: ...OK"
        }
        if (starts(s, "NO DATA")) return ELM_NO_DATA;
        if (starts(s, "UNABLE")) return ELM_UNABLE;
        if (starts(s, "STOPPED")) return ELM_STOPPED;
        if (strcmp(s, "?") == 0) return ELM_UNKNOWN_CMD;
        if (strstr(s, "ERROR") || starts(s, "BUFFER FULL")) return ELM_BUS_ERROR;
        if (is_echo(s, echo_cmd)) continue;

        // linha de dados: pares hex, com ou sem espacos. Ignora prefixo "0:" (multi-quadro CAN)
        char *colon = strchr(s, ':');
        if (colon && colon - s <= 2) s = colon + 1;
        int hi = -1, bad = 0;
        size_t start_n = *n;
        for (char *c = s; *c; c++) {
            if (*c == ' ') continue;
            int v = hexval(*c);
            if (v < 0) { bad = 1; break; }
            if (hi < 0) hi = v;
            else {
                if (*n < max) out[(*n)++] = (uint8_t)((hi << 4) | v);
                hi = -1;
            }
        }
        if (bad || hi >= 0) *n = start_n;  // linha que nao e hex (texto qualquer): ignora
    }
    if (!any_line) return ELM_EMPTY;
    if (*n == 0) return ELM_EMPTY;
    if (out[0] == 0x7F) return ELM_NEGATIVE;
    return ELM_OK;
}

const char *elm_status_str(elm_status_t s)
{
    switch (s) {
    case ELM_OK: return "OK";
    case ELM_NO_DATA: return "SEM RESPOSTA";
    case ELM_UNABLE: return "NAO CONECTOU";
    case ELM_UNKNOWN_CMD: return "COMANDO ?";
    case ELM_BUS_ERROR: return "ERRO NA LINHA";
    case ELM_STOPPED: return "INTERROMPIDO";
    case ELM_NEGATIVE: return "RESPOSTA NEGATIVA";
    case ELM_TIMEOUT: return "TIMEOUT";
    default: return "VAZIO";
    }
}
