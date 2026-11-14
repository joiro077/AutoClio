#include "obd2_decode.h"
#include <stdio.h>

bool obd2_decode_mode01(const uint8_t *r, size_t len, vehicle_sample_t *s)
{
    if (len < 3 || r[0] != 0x41) return false;
    uint8_t pid = r[1];
    float A = r[2], B = len > 3 ? r[3] : 0;
    switch (pid) {
    case 0x04: s->load_pct = A * 100.0f / 255.0f; return true;
    case 0x05: s->coolant_c = A - 40.0f; return true;
    case 0x06: s->stft_pct = (A - 128.0f) * 100.0f / 128.0f; return true;
    case 0x07: s->ltft_pct = (A - 128.0f) * 100.0f / 128.0f; return true;
    case 0x0B: s->map_kpa = A; return true;
    case 0x0C: if (len < 4) return false; s->rpm = (A * 256.0f + B) / 4.0f; return true;
    case 0x0D: s->speed_kmh = A; return true;
    case 0x0F: s->iat_c = A - 40.0f; return true;
    case 0x10: if (len < 4) return false; s->maf_gs = (A * 256.0f + B) / 100.0f; return true;
    case 0x11: s->tps_pct = A * 100.0f / 255.0f; return true;
    case 0x14: s->o2_v = A / 200.0f; return true;      // sonda banco 1, sensor 1 (antes do catalisador)
    case 0x33: s->baro_kpa = A; return true;
    case 0x42: if (len < 4) return false; s->vbat = (A * 256.0f + B) / 1000.0f; return true;
    default: return false;
    }
}

void obd2_parse_supported(const uint8_t *r, size_t len, obd2_supported_t *sup)
{
    if (len < 6 || r[0] != 0x41) return;
    int base = r[1];
    if (base != 0x00 && base != 0x20 && base != 0x40) return;
    sup->mask[base / 0x20] = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 8) | r[5];
}

bool obd2_is_supported(const obd2_supported_t *sup, uint8_t pid)
{
    if (pid == 0 || pid > 0x60) return false;
    int blk = (pid - 1) / 0x20;
    int bit = (pid - 1) % 0x20;
    return (sup->mask[blk] >> (31 - bit)) & 1u;
}

int obd2_decode_dtcs(const uint8_t *r, size_t len, uint8_t mode_resp, char codes[][6], int max)
{
    static const char letter[4] = { 'P', 'C', 'B', 'U' };
    int n = 0;
    if (len < 1 || r[0] != mode_resp) return 0;
    // K-Line: cada quadro tem 7 bytes (modo + 3 codigos). Nao da para procurar
    // 0x43 no meio dos dados porque um codigo pode conter 0x43 (ex.: C0300).
    size_t frame = (len % 7 == 0) ? 7 : len;
    for (size_t f = 0; f + 1 < len; f += frame) {
        if (r[f] != mode_resp) break;
        for (size_t i = f + 1; i + 1 < f + frame && i + 1 < len; i += 2) {
            uint8_t a = r[i], b = r[i + 1];
            if (a == 0 && b == 0) continue;
            if (n < max) {
                snprintf(codes[n], 6, "%c%01X%01X%02X", letter[a >> 6], (a >> 4) & 3, a & 0xF, b);
                n++;
            }
        }
    }
    return n;
}
