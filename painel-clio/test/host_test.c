// Testes da logica pura no PC:  make -C test
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/core/ecu_profiles.h"
#include "../main/core/elm_parse.h"
#include "../main/core/fuel.h"
#include "../main/core/gear.h"
#include "../main/core/injmon.h"
#include "../main/core/obd2_decode.h"
#include "../main/core/trip.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static int find_profile(const char *prefix)
{
    for (int i = 0; i < g_ecu_profile_count; i++)
        if (strncmp(g_ecu_profiles[i].name, prefix, strlen(prefix)) == 0) return i;
    return -1;
}

static void test_elm_parse(void)
{
    uint8_t b[64]; size_t n;
    CHECK(elm_parse_hex("41 0C 1A F8 \r\r>", "010C", b, sizeof b, &n) == ELM_OK);
    CHECK(n == 4 && b[2] == 0x1A && b[3] == 0xF8);
    CHECK(elm_parse_hex("BUS INIT: ...OK\r41 0D 32\r\r>", "010D", b, sizeof b, &n) == ELM_OK && n == 3);
    CHECK(elm_parse_hex("010C\r410C0FA0\r>", "010C", b, sizeof b, &n) == ELM_OK && n == 4); // eco + sem espacos
    CHECK(elm_parse_hex("NO DATA\r\r>", "010C", b, sizeof b, &n) == ELM_NO_DATA);
    CHECK(elm_parse_hex("BUS INIT: ...ERROR\r>", "0100", b, sizeof b, &n) == ELM_UNABLE);
    CHECK(elm_parse_hex("7F 21 12\r>", "21A9", b, sizeof b, &n) == ELM_NEGATIVE);
    CHECK(elm_parse_hex("?\r>", "ATXX", b, sizeof b, &n) == ELM_UNKNOWN_CMD);
}

static void test_obd2(void)
{
    vehicle_sample_t s; vehicle_sample_clear(&s);
    uint8_t rpm[] = { 0x41, 0x0C, 0x1A, 0xF8 };
    CHECK(obd2_decode_mode01(rpm, 4, &s)); NEAR(s.rpm, 1726, 0.1);
    uint8_t t[] = { 0x41, 0x05, 0x7B };
    obd2_decode_mode01(t, 3, &s); NEAR(s.coolant_c, 83, 0.01);
    uint8_t sup[] = { 0x41, 0x00, 0xBE, 0x1F, 0xA8, 0x13 };
    obd2_supported_t m = { 0 }; obd2_parse_supported(sup, 6, &m);
    CHECK(obd2_is_supported(&m, 0x01)); CHECK(!obd2_is_supported(&m, 0x02));
    CHECK(obd2_is_supported(&m, 0x0C)); CHECK(obd2_is_supported(&m, 0x0D));
    char c[8][6];
    uint8_t d[] = { 0x43, 0x01, 0x33, 0x43, 0x00, 0x00, 0x00, 0x43, 0x01, 0x71, 0x00, 0x00, 0x00, 0x00 };
    int k = obd2_decode_dtcs(d, sizeof d, 0x43, c, 8);
    CHECK(k == 3);
    CHECK(strcmp(c[0], "P0133") == 0); CHECK(strcmp(c[1], "C0300") == 0); CHECK(strcmp(c[2], "P0171") == 0);
}

static void test_kwp(void)
{
    int i = find_profile("SIRIUS32 - EMS3132");
    CHECK(i >= 0);
    if (i < 0) return;
    const ecu_profile_t *p = &g_ecu_profiles[i];
    CHECK(!p->fastinit);   // ddt4all marca esta como init 5 baud
    // 61 A9: rpm=0x0BB8(3000) bateria=(200*0.032+8)=14.4 MAP=(150*3.7+103)/10=65.8kPa
    // borboleta 50*0.4=20 graus agua (200*0.625-40)=85 ar (110*.625-40)=28.75 vel=80
    uint8_t r[26] = { 0x61, 0xA9, 0x0B, 0xB8, 200, 150, 50, 200, 110, 0, 0, 80 };
    vehicle_sample_t s; vehicle_sample_clear(&s);
    int n = kwp_decode_frame(p, r, sizeof r, &s);
    CHECK(n >= 7);
    NEAR(s.rpm, 3000, 0.1); NEAR(s.vbat, 14.4, 0.01); NEAR(s.map_kpa, 65.8, 0.01);
    NEAR(s.coolant_c, 85, 0.01); NEAR(s.iat_c, 28.75, 0.01); NEAR(s.speed_kmh, 80, 0.01);
    // 61 AA: inj b8-9 = 1000*0.004 = 4ms ; consumo b13-14 = 650/100 = 6.5 L/h
    uint8_t q[16] = { 0x61, 0xAA, 0x0B, 0xB8, 0, 0, 0, 0x03, 0xE8, 0, 0, 0, 0x02, 0x8A };
    kwp_decode_frame(p, q, sizeof q, &s);
    NEAR(s.inj_ms, 4.0, 0.001); NEAR(s.fuel_lph_ecu, 6.5, 0.001);
    CHECK(kwp_rpm_lid(p) == 0xA9);

    // identificacao: soft 00E1 versao 8000 fornecedor "001"
    uint8_t id[26] = { 0x61, 0x80, 1, 2, 3, 4, 5, 8, '0', '0', '1', 0, 0, 0, 0, 0, 0x00, 0xE1, 0x80, 0x00 };
    ecu_id_info_t info;
    CHECK(kwp_parse_ident(id, sizeof id, &info));
    CHECK(strcmp(info.soft, "00E1") == 0 && strcmp(info.version, "8000") == 0);
    CHECK(kwp_match_profile(&info) == i);
    // Sirius 32 soft RE0 (00E0) com versao desconhecida tambem cai num perfil Sirius32 com o mesmo layout
    uint8_t id2[26] = { 0x61, 0x80, 1, 2, 3, 4, 5, 25, '0', '0', '1', 0, 0, 0, 0, 0, 0x00, 0xE0, 0x47, 0x00 };
    CHECK(kwp_parse_ident(id2, sizeof id2, &info));
    int j = kwp_match_profile(&info);
    CHECK(j >= 0 && strncmp(g_ecu_profiles[j].name, "SIRIUS32 - EMS3132", 18) == 0);
    if (j >= 0) {
        vehicle_sample_t s2; vehicle_sample_clear(&s2);
        kwp_decode_frame(&g_ecu_profiles[j], r, sizeof r, &s2);
        NEAR(s2.rpm, 3000, 0.1); NEAR(s2.coolant_c, 85, 0.01);
    }
}

static void test_fuel_trip(void)
{
    settings_t c; settings_defaults(&c);
    vehicle_sample_t s; vehicle_sample_clear(&s);
    fuel_src_t src;
    s.rpm = 2500; s.fuel_lph_ecu = 6.0f;
    NEAR(fuel_lph(&s, &c, &src), 6.0, 1e-4); CHECK(src == FUEL_SRC_ECU);
    s.fuel_lph_ecu = VNAN; s.inj_ms = 3.8f;
    float l = fuel_lph(&s, &c, &src); CHECK(src == FUEL_SRC_INJ);
    // 4 cil * 1250 inj/min * 60 = 300000 inj/h * 3.0ms * 160/60000 cc/ms = 2400 cc/h
    NEAR(l, 2.4, 0.01);
    s.inj_ms = VNAN; s.map_kpa = 60; s.iat_c = 30; s.tps_pct = 20;
    l = fuel_lph(&s, &c, &src); CHECK(src == FUEL_SRC_MAP); CHECK(l > 2 && l < 8);
    printf("  speed-density 2500rpm 60kPa = %.2f L/h\n", l);
    NEAR(fuel_instant_display(6.0f, 90.0f, 1), 15.0, 1e-4);
    NEAR(fuel_instant_display(6.0f, 100.0f, 0), 6.0, 1e-4);

    trip_state_t t; trip_init(&t); trip_start(&t);
    for (int k = 0; k < 3600; k++) trip_update(&t, 60.0f, 4.0f, 1.0f); // 1h a 60km/h e 4 L/h
    NEAR(t.current.dist_km, 60, 1e-6); NEAR(t.current.fuel_l, 4, 1e-6);
    NEAR(trip_kml(&t.current), 15, 1e-6); NEAR(trip_avg_speed(&t.current), 60, 1e-6);
    double left;
    NEAR(trip_range_km(&t, 50, 10, &left), 46 * 15, 1e-6); NEAR(left, 46, 1e-6);
    trip_update(&t, 60, 4, 30.0f);               // lacuna de 30 s: ignora
    NEAR(t.current.dist_km, 60, 1e-6);
    trip_end(&t); NEAR(t.p.last_trip.dist_km, 60, 1e-6);
    // abasteceu 4.4 L e o painel contou 4.0 -> fator 1.1
    trip_state_t t2 = t; t2.p.since_fill.fuel_l = 40;
    NEAR(trip_calibrate_factor(&t2, 44, 1.0), 1.1, 1e-9);
}

static void test_gear_shift(void)
{
    settings_t c; settings_defaults(&c);
    // relacoes de exemplo (rpm por km/h), so para teste
    float r[5] = { 130, 72, 49, 37, 30 };
    for (int i = 0; i < 5; i++) c.gear_ratio[i] = r[i];
    CHECK(gear_detect(&c, 2000, 2000 / 49.0f) == 3);
    CHECK(gear_detect(&c, 3000, 3000 / 30.5f) == 5);
    CHECK(gear_detect(&c, 3000, 3000 / 60.0f) == 0);  // embreagem/entre marchas
    CHECK(gear_detect(&c, 800, 0) == 0);

    gear_learn_acc_t a = { 0 };
    for (int k = 0; k < 20; k++) gear_learn_add(&a, 2500, 2500 / 37.0f);
    settings_t c2; settings_defaults(&c2);
    CHECK(gear_learn_commit(&c2, 4, &a)); NEAR(c2.gear_ratio[3], 37, 0.01);

    // ECO: 3a a 2600 rpm, carga leve -> subir (4a ficaria 1963 rpm)
    vehicle_sample_t s; vehicle_sample_clear(&s);
    s.rpm = 2600; s.map_kpa = 45; s.baro_kpa = 95;
    shift_state_t st = { .last_rpm = 2600 };
    CHECK(shift_update(&st, &c, &s, 3, 0.1f) == SHIFT_UP);
    // ECO: subida, 4a, pe no fundo e rpm caindo -> reduzir
    shift_state_t st2 = { .last_rpm = 1800 };
    s.map_kpa = 94; s.tps_pct = 100;
    shift_advice_t adv = SHIFT_NONE;
    float rpm = 1800;
    for (int k = 0; k < 30; k++) { rpm -= 10; s.rpm = rpm; adv = shift_update(&st2, &c, &s, 4, 0.1f); }
    CHECK(adv == SHIFT_DOWN);

    // SPORT com curva de torque "tipica" de aspirado (dados ficticios de teste)
    c.shift_mode = SHIFT_SPORT;
    float tq[TQ_BINS] = { 110, 120, 128, 135, 142, 148, 150, 148, 145, 140, 133, 124, 112, 100 };
    memcpy(c.tq_curve, tq, sizeof tq);
    float up12 = shift_optimal_upshift_rpm(&c, 1);
    printf("  sport: subida otima 1a->2a = %.0f rpm (pico de torque do teste = 4000)\n", up12);
    CHECK(up12 > 4500);   // bem depois do pico de torque
}

static void test_injmon(void)
{
    injmon_t m; injmon_init(&m);
    vehicle_sample_t s; vehicle_sample_clear(&s);
    s.rpm = 2000; s.coolant_c = 90; s.map_kpa = 50; s.baro_kpa = 100; s.inj_ms = 3.0f;
    for (int k = 0; k < 100; k++) injmon_update(&m, &s, 0.2f);
    CHECK(m.status == INJ_OK);
    s.inj_ms = 4.0f;  // +33 %
    for (int k = 0; k < 40; k++) injmon_update(&m, &s, 0.2f);
    CHECK(m.status == INJ_HIGH);
}

int main(void)
{
    printf("perfis de ECU embutidos: %d\n", g_ecu_profile_count);
    test_elm_parse();
    test_obd2();
    test_kwp();
    test_fuel_trip();
    test_gear_shift();
    test_injmon();
    printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", fails);
    return fails != 0;
}
