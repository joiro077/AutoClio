// ui.c - telas: painel, menu, editor numerico, falhas (DTC), dados ao vivo e relatorio
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "display.h"
#include "lvgl.h"
#include "obd_task.h"

#define C_BG     lv_color_hex(0x0B0E12)
#define C_PANEL  lv_color_hex(0x161B22)
#define C_TEXT   lv_color_hex(0xE6EDF3)
#define C_DIM    lv_color_hex(0x8B949E)
#define C_ACCENT lv_color_hex(0x2F81F7)
#define C_GOOD   lv_color_hex(0x3FB950)
#define C_WARN   lv_color_hex(0xD29922)
#define C_BAD    lv_color_hex(0xF85149)

#define RPM_MAX 7000

LV_FONT_DECLARE(font_big88);   // gerada por tools/gen_bigfont.ps1

static lv_obj_t *scr_dash, *scr_menu, *scr_edit, *scr_dtc, *scr_data, *scr_mix, *scr_ev;

// indicadores de status (coluna da esquerda do painel) e tela de eventos do A/C
#define NEEDLE_LEN 150
#define EV_SHOW 12
static lv_obj_t *ind_box[7], *ind_name[7], *ind_val[7], *ev_lbl;

// mistura
static lv_obj_t *ch_o2, *ch_warm, *mix_lbl;
static lv_chart_series_t *ser_o2, *ser_temp, *ser_trim, *ser_loop;
static int warm_n;

// painel
static lv_obj_t *scale, *needle, *lbl_rpm, *lbl_speed, *lbl_gear, *lbl_shift;
static lv_obj_t *lbl_temp, *lbl_inst, *lbl_avg_trip, *lbl_avg_reset, *lbl_range, *lbl_status, *lbl_alert;
static lv_obj_t *bar_load, *lbl_load, *btn_sport, *shift_flash;
static float disp_rpm = 0;

// editor
typedef void (*edit_apply_cb)(float v);
static lv_obj_t *edit_title, *edit_ta;
static edit_apply_cb edit_cb;

// dtc / dados / relatorio
static lv_obj_t *dtc_list, *dtc_msg, *data_lbl, *report_box;
static uint32_t seen_dtc_seq, seen_report_seq;

// menu
static lv_obj_t *menu_list;
static void menu_rebuild(void);
static void ev_go_mix(lv_event_t *e);
static void ev_go_ev(lv_event_t *e);
static void ev_go_steer(lv_event_t *e);

/* ------------------------------------------------------------ utilitarios */
static lv_obj_t *new_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, C_BG, 0);
    lv_obj_set_style_text_color(s, C_TEXT, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, C_PANEL, 0);
    lv_obj_set_style_border_color(b, C_ACCENT, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = label(b, &lv_font_montserrat_20, C_TEXT, txt);
    lv_obj_center(l);
    return b;
}

static void fmt_or_dash(char *buf, size_t n, const char *fmt, double v)
{
    if (isnan(v) || isinf(v)) snprintf(buf, n, "--");
    else snprintf(buf, n, fmt, v);
}

static void go(lv_obj_t *scr) { lv_screen_load(scr); }
static void ev_go_dash(lv_event_t *e) { go(scr_dash); }
static void ev_go_menu(lv_event_t *e) { menu_rebuild(); lv_obj_scroll_to_y(menu_list, 0, LV_ANIM_OFF); go(scr_menu); }

// alterna o brilho entre dia (100 %) e noite (10 %)
static void ev_night(lv_event_t *e)
{
    app_lock();
    g_cfg.brightness = (g_cfg.brightness > 10) ? 10 : 100;
    int b = g_cfg.brightness;
    app_unlock();
    display_set_brightness(b);
    logic_post(LCMD_SAVE_SETTINGS, 0);
}

// liga/desliga o modo Sport do indicador de troca (ao desligar volta para Eco)
static void ev_sport(lv_event_t *e)
{
    app_lock();
    g_cfg.shift_mode = (g_cfg.shift_mode == SHIFT_SPORT) ? SHIFT_ECO : SHIFT_SPORT;
    app_unlock();
    logic_post(LCMD_SAVE_SETTINGS, 0);
}
static void ev_go_dtc(lv_event_t *e) { go(scr_dtc); }
static void ev_go_data(lv_event_t *e) { go(scr_data); }

/* ------------------------------------------------------------ painel */
static lv_obj_t *info_row(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);      // as linhas dividem a altura que sobra
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(row, &lv_font_montserrat_20, C_DIM, name);
    return label(row, &lv_font_montserrat_28, C_TEXT, "--");
}

static void make_ind(int i, const char *name)
{
    ind_box[i] = lv_obj_create(scr_dash);
    lv_obj_set_size(ind_box[i], 58, 54);
    lv_obj_set_pos(ind_box[i], 2, 6 + i * 58);
    lv_obj_set_style_bg_color(ind_box[i], C_PANEL, 0);
    lv_obj_set_style_border_width(ind_box[i], 0, 0);
    lv_obj_set_style_radius(ind_box[i], 6, 0);
    lv_obj_set_style_pad_all(ind_box[i], 0, 0);
    lv_obj_remove_flag(ind_box[i], LV_OBJ_FLAG_SCROLLABLE);
    ind_name[i] = label(ind_box[i], &lv_font_montserrat_14, C_DIM, name);
    lv_obj_align(ind_name[i], LV_ALIGN_TOP_MID, 0, 3);
    ind_val[i] = label(ind_box[i], &lv_font_montserrat_20, C_TEXT, "--");
    lv_obj_align(ind_val[i], LV_ALIGN_BOTTOM_MID, 0, -4);
}

// hl = quadradinho aceso (fundo colorido, texto branco)
static void set_ind(int i, lv_color_t bg, bool hl, const char *txt)
{
    lv_obj_set_style_bg_color(ind_box[i], bg, 0);
    lv_obj_set_style_text_color(ind_val[i], hl ? lv_color_white() : C_TEXT, 0);
    lv_obj_set_style_text_color(ind_name[i], hl ? lv_color_white() : C_DIM, 0);
    lv_label_set_text(ind_val[i], txt);
    lv_obj_align(ind_val[i], LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void update_ind(const vehicle_sample_t *s)
{
    const lv_color_t orange = lv_color_hex(0xFF8800);
    bool ok = s->ecu_online;
    char b[16];

    // 0: A/C. ON = a ECU comanda o compressor. CUT (vermelho) = pedido ligado e a ECU nao
    // liga com a borboleta quase toda aberta; "..." (laranja) = pedido ligado, ainda nao ligou.
    if (ok && VALID(s->ac_cmd)) {
        bool req = VALID(s->ac_req) && s->ac_req > 0.5f;
        if (s->ac_cmd > 0.5f) set_ind(0, C_GOOD, true, "ON");
        else if (req) {
            bool cut = VALID(s->tps_pct) && s->tps_pct >= 80.0f;
            set_ind(0, cut ? C_BAD : orange, true, cut ? "CUT" : "...");
        } else set_ind(0, C_PANEL, false, "OFF");
    } else set_ind(0, C_PANEL, false, "--");

    // 1: ventoinha do radiador
    if (ok && (VALID(s->fan1) || VALID(s->fan2))) {
        if (VALID(s->fan2) && s->fan2 > 0.5f) set_ind(1, orange, true, "V2");
        else if (VALID(s->fan1) && s->fan1 > 0.5f) set_ind(1, C_ACCENT, true, "V1");
        else set_ind(1, C_PANEL, false, "OFF");
    } else set_ind(1, C_PANEL, false, "--");

    // 2..5: valores. O DDT4All da Sirius 32 tem so o sensor de temperatura do ar (o do coletor).
    fmt_or_dash(b, sizeof b, "%.0fC", ok ? s->iat_c : NAN);
    set_ind(2, C_PANEL, false, b);
    fmt_or_dash(b, sizeof b, "%.0f\xC2\xB0", ok ? s->spark_adv : NAN);    // o degree sign existe na fonte Montserrat
    set_ind(3, C_PANEL, false, b);
    fmt_or_dash(b, sizeof b, "%.0f%%", ok ? s->tps_pct : NAN);
    set_ind(4, C_PANEL, false, b);
    fmt_or_dash(b, sizeof b, "%.2f", ok ? s->o2_v : NAN);
    set_ind(5, C_PANEL, false, b);

    // 6: pressostato da direcao. O DDT4All chama o nivel 1 de "ACTIF": situacao de manobra, aceso em laranja.
    if (ok && VALID(s->ps_level)) {
        if (s->ps_level > 0.5f) set_ind(6, orange, true, "ON");
        else set_ind(6, C_PANEL, false, "OFF");
    } else set_ind(6, C_PANEL, false, "--");
}

static void build_dash(void)
{
    scr_dash = new_screen();

    // conta-giros
    scale = lv_scale_create(scr_dash);
    lv_obj_set_size(scale, 396, 396);       // menor, para sobrar a coluna de indicadores a esquerda
    lv_obj_set_pos(scale, 62, 10);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(scale, 0, RPM_MAX);
    lv_scale_set_total_tick_count(scale, 29);      // a cada 250 rpm
    lv_scale_set_major_tick_every(scale, 4);       // numero a cada 1000
    lv_scale_set_label_show(scale, true);
    lv_scale_set_angle_range(scale, 260);
    lv_scale_set_rotation(scale, 140);
    static const char *rpm_txt[] = { "0", "1", "2", "3", "4", "5", "6", "7", NULL };
    lv_scale_set_text_src(scale, rpm_txt);
    lv_obj_set_style_bg_opa(scale, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scale, C_PANEL, 0);
    lv_obj_set_style_radius(scale, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_font(scale, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(scale, C_TEXT, 0);
    lv_obj_set_style_length(scale, 22, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 10, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, C_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, C_DIM, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(scale, 14, 0);

    // faixa vermelha (SUPOSICAO: ajuste ao corte do seu motor)
    static lv_style_t st_red_main, st_red_ticks;
    lv_style_init(&st_red_main);
    lv_style_set_arc_color(&st_red_main, C_BAD);
    lv_style_set_arc_width(&st_red_main, 8);
    lv_style_init(&st_red_ticks);
    lv_style_set_line_color(&st_red_ticks, C_BAD);
    lv_scale_section_t *sec = lv_scale_add_section(scale);
    lv_scale_section_set_range(sec, 6000, RPM_MAX);
    lv_scale_section_set_style(sec, LV_PART_MAIN, &st_red_main);
    lv_scale_section_set_style(sec, LV_PART_INDICATOR, &st_red_ticks);
    lv_scale_section_set_style(sec, LV_PART_ITEMS, &st_red_ticks);

    needle = lv_line_create(scale);
    lv_obj_set_style_line_width(needle, 6, 0);
    lv_obj_set_style_line_color(needle, C_BAD, 0);
    lv_obj_set_style_line_rounded(needle, true, 0);
    lv_scale_set_line_needle_value(scale, needle, NEEDLE_LEN, 0);

    lbl_rpm = label(scale, &lv_font_montserrat_48, C_TEXT, "0");
    lv_obj_align(lbl_rpm, LV_ALIGN_CENTER, 0, 90);
    lv_obj_t *u = label(scale, &lv_font_montserrat_14, C_DIM, "rpm");
    lv_obj_align(u, LV_ALIGN_CENTER, 0, 130);

    // carga do motor (0..100 %): quanto do torque disponivel NESTE giro esta em uso
    lv_obj_t *lc = label(scr_dash, &lv_font_montserrat_14, C_DIM, "CARGA");
    lv_obj_set_pos(lc, 14, 454);
    bar_load = lv_bar_create(scr_dash);
    lv_obj_set_pos(bar_load, 74, 450);
    lv_obj_set_size(bar_load, 262, 22);
    lv_bar_set_range(bar_load, 0, 100);
    lv_obj_set_style_bg_color(bar_load, C_PANEL, 0);
    lv_obj_set_style_bg_opa(bar_load, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar_load, 4, 0);
    lv_obj_set_style_radius(bar_load, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_load, C_GOOD, LV_PART_INDICATOR);
    lbl_load = label(scr_dash, &lv_font_montserrat_20, C_TEXT, "--");
    lv_obj_set_pos(lbl_load, 346, 449);

    // coluna de indicadores de status, no canto esquerdo
    static const char *ind_names[7] = { "A/C", "VENT", "ADM", "AVC", "BORB", "SONDA", "DIR" };
    for (int i = 0; i < 7; i++) make_ind(i, ind_names[i]);

    // coluna da direita
    lv_obj_t *col = lv_obj_create(scr_dash);
    lv_obj_set_pos(col, 465, 10);
    lv_obj_set_size(col, 325, 400);
    lv_obj_set_style_bg_color(col, C_PANEL, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    // velocidade e marcha em fonte grande (font_big88: so "-0123456789AN")
    lv_obj_t *top = lv_obj_create(col);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 100);
    lbl_speed = label(top, &font_big88, C_TEXT, "0");
    lv_obj_align(lbl_speed, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *kmh = label(top, &lv_font_montserrat_20, C_DIM, "km/h");
    lv_obj_align(kmh, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lbl_gear = label(top, &font_big88, C_ACCENT, "-");
    lv_obj_align(lbl_gear, LV_ALIGN_TOP_RIGHT, -48, 0);
    lv_obj_t *mk = label(top, &lv_font_montserrat_20, C_DIM, "marcha");
    lv_obj_align(mk, LV_ALIGN_BOTTOM_RIGHT, -40, 0);
    lbl_shift = label(top, &lv_font_montserrat_48, C_GOOD, "");
    lv_obj_align(lbl_shift, LV_ALIGN_TOP_RIGHT, 0, 8);

    lbl_temp = info_row(col, "Temp. motor");
    lbl_inst = info_row(col, "Consumo inst.");
    lbl_avg_trip = info_row(col, "Media trajeto");
    lbl_avg_reset = info_row(col, "Media geral");
    lbl_range = info_row(col, "Autonomia");

    lbl_status = label(scr_dash, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_pos(lbl_status, 470, 415);
    lv_obj_set_width(lbl_status, 320);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);

    lv_obj_t *b;
    // botao da lua: alterna o brilho entre 100 % e 10 % (a fonte nao tem simbolo
    // de lua, entao ela e desenhada com dois circulos sobrepostos)
    b = button(scr_dash, "", ev_night, NULL);        lv_obj_set_size(b, 55, 40); lv_obj_set_pos(b, 465, 435);
    lv_obj_t *moon = lv_obj_create(b);
    lv_obj_remove_style_all(moon);
    lv_obj_set_size(moon, 24, 24);
    lv_obj_set_style_radius(moon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(moon, lv_color_hex(0xF2CC60), 0);
    lv_obj_set_style_bg_opa(moon, LV_OPA_COVER, 0);
    lv_obj_remove_flag(moon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(moon, LV_ALIGN_CENTER, -2, 0);
    lv_obj_t *cut = lv_obj_create(b);
    lv_obj_remove_style_all(cut);
    lv_obj_set_size(cut, 20, 20);
    lv_obj_set_style_radius(cut, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cut, C_PANEL, 0);
    lv_obj_set_style_bg_opa(cut, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cut, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(cut, LV_ALIGN_CENTER, 5, -4);

    b = button(scr_dash, "Menu", ev_go_menu, NULL);  lv_obj_set_size(b, 82, 40); lv_obj_set_pos(b, 528, 435);
    b = button(scr_dash, "Falhas", ev_go_dtc, NULL); lv_obj_set_size(b, 82, 40); lv_obj_set_pos(b, 618, 435);
    b = button(scr_dash, "Dados", ev_go_data, NULL); lv_obj_set_size(b, 82, 40); lv_obj_set_pos(b, 708, 435);

    // botao Sport, no canto livre acima do conta-giros (fica vermelho quando ligado)
    btn_sport = button(scr_dash, "SPORT", ev_sport, NULL);
    lv_obj_set_size(btn_sport, 84, 40);
    lv_obj_set_pos(btn_sport, 376, 6);

    // aviso de troca do modo Sport: tela inteira vermelha piscando, seta e "SHIFT".
    // Nao e clicavel, entao os botoes de baixo continuam funcionando.
    shift_flash = lv_obj_create(scr_dash);
    lv_obj_remove_style_all(shift_flash);
    lv_obj_set_size(shift_flash, 800, 480);
    lv_obj_set_pos(shift_flash, 0, 0);
    lv_obj_set_style_bg_color(shift_flash, C_BAD, 0);
    lv_obj_set_style_bg_opa(shift_flash, LV_OPA_COVER, 0);
    lv_obj_remove_flag(shift_flash, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    static const lv_point_precise_t chevron[] = { { 20, 150 }, { 180, 20 }, { 340, 150 } };
    lv_obj_t *arrow = lv_line_create(shift_flash);
    lv_line_set_points(arrow, chevron, 3);
    lv_obj_set_style_line_width(arrow, 36, 0);
    lv_obj_set_style_line_color(arrow, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(arrow, true, 0);
    lv_obj_align(arrow, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_t *st = label(shift_flash, &font_big88, lv_color_white(), "SHIFT");
    lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -90);
    lv_obj_add_flag(shift_flash, LV_OBJ_FLAG_HIDDEN);

    lbl_alert = label(scr_dash, &lv_font_montserrat_20, lv_color_white(), "");
    lv_obj_set_style_bg_color(lbl_alert, C_BAD, 0);
    lv_obj_set_style_bg_opa(lbl_alert, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(lbl_alert, 6, 0);
    lv_obj_align(lbl_alert, LV_ALIGN_TOP_LEFT, 20, 4);
    lv_obj_add_flag(lbl_alert, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------ relatorio */
static void ev_report_ok(lv_event_t *e)
{
    logic_post(LCMD_REPORT_SEEN, 0);
    lv_obj_add_flag(report_box, LV_OBJ_FLAG_HIDDEN);
}

static void show_report(const dash_t *d)
{
    if (!report_box) {
        report_box = lv_obj_create(lv_layer_top());
        lv_obj_set_size(report_box, 520, 400);
        lv_obj_center(report_box);
        lv_obj_set_style_bg_color(report_box, C_PANEL, 0);
        lv_obj_set_style_border_color(report_box, C_ACCENT, 0);
        lv_obj_set_style_border_width(report_box, 2, 0);
        lv_obj_set_style_text_color(report_box, C_TEXT, 0);
        lv_obj_remove_flag(report_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *t = label(report_box, &lv_font_montserrat_28, C_ACCENT, "Relatorio da viagem");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_t *body = label(report_box, &lv_font_montserrat_28, C_TEXT, "");
        lv_obj_align(body, LV_ALIGN_TOP_LEFT, 10, 50);
        lv_obj_set_user_data(report_box, body);
        lv_obj_t *b = button(report_box, "OK", ev_report_ok, NULL);
        lv_obj_set_size(b, 140, 50);
        lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    const trip_accum_t *a = &d->last_trip;
    app_lock();
    float price = g_cfg.fuel_price;
    app_unlock();
    double kml = trip_kml(a), vm = trip_avg_speed(a);
    int mins = (int)(a->time_s / 60);
    char s_kml[16], s_vm[16];
    fmt_or_dash(s_kml, sizeof s_kml, "%.1f km/L", kml);
    fmt_or_dash(s_vm, sizeof s_vm, "%.0f km/h", vm);
    static char txt[256];   // snprintf da libc: o printf do LVGL esta sem %f
    snprintf(txt, sizeof txt,
                          "Distancia:    %.1f km\n"
                          "Tempo:        %dh%02d\n"
                          "Vel. media:   %s\n"
                          "Consumo:      %s\n"
                          "Combustivel:  %.2f L\n"
                          "Custo:        R$ %.2f",
                          a->dist_km, mins / 60, mins % 60, s_vm, s_kml, a->fuel_l, a->fuel_l * price);
    lv_label_set_text((lv_obj_t *)lv_obj_get_user_data(report_box), txt);
    lv_obj_remove_flag(report_box, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------ editor numerico */
static void ev_edit_kb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        float v = 0;
        const char *txt = lv_textarea_get_text(edit_ta);
        char tmp[24];
        snprintf(tmp, sizeof tmp, "%s", txt);
        for (char *p = tmp; *p; p++) if (*p == ',') *p = '.';
        if (sscanf(tmp, "%f", &v) == 1 && edit_cb) edit_cb(v);
        menu_rebuild();
        go(scr_menu);
    } else if (code == LV_EVENT_CANCEL) {
        go(scr_menu);
    }
}

static void build_edit(void)
{
    scr_edit = new_screen();
    edit_title = label(scr_edit, &lv_font_montserrat_28, C_TEXT, "");
    lv_obj_align(edit_title, LV_ALIGN_TOP_MID, 0, 10);
    edit_ta = lv_textarea_create(scr_edit);
    lv_textarea_set_one_line(edit_ta, true);
    lv_textarea_set_accepted_chars(edit_ta, "0123456789.,");
    lv_textarea_set_max_length(edit_ta, 10);
    lv_obj_set_style_text_font(edit_ta, &lv_font_montserrat_48, 0);
    lv_obj_set_width(edit_ta, 360);
    lv_obj_align(edit_ta, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_t *kb = lv_keyboard_create(scr_edit);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, edit_ta);
    lv_obj_set_size(kb, 800, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, ev_edit_kb, LV_EVENT_ALL, NULL);
}

static void open_edit(const char *title, float current, const char *fmt, edit_apply_cb cb)
{
    char buf[24];
    lv_label_set_text(edit_title, title);
    snprintf(buf, sizeof buf, fmt, current);
    lv_textarea_set_text(edit_ta, buf);
    edit_cb = cb;
    go(scr_edit);
}

/* ------------------------------------------------------------ menu */
static void save_cfg(void) { logic_post(LCMD_SAVE_SETTINGS, 0); }

static void ap_price(float v)  { app_lock(); if (v > 0 && v < 50) g_cfg.fuel_price = v; app_unlock(); save_cfg(); }
static void ap_tank(float v)   { app_lock(); if (v >= 20 && v <= 120) g_cfg.tank_l = v; app_unlock(); save_cfg(); }
static void ap_calib(float v)  { logic_post(LCMD_CALIBRATE, v); }
static void ap_learn(float v)  { int g = (int)v; if (g >= 1 && g <= MAX_GEARS) logic_post(LCMD_LEARN_GEAR, (float)g); }
static void ap_sport(float v)  { app_lock(); if (v > 2000 && v < 8000) g_cfg.sport_up_rpm = v; app_unlock(); save_cfg(); }
static void ap_ecoup(float v)  { app_lock(); if (v > 1200 && v < 5000) g_cfg.eco_up_rpm = v; app_unlock(); save_cfg(); }
static void ap_redline(float v){ app_lock(); if (v > 4000 && v < 8000) g_cfg.redline_rpm = v; app_unlock(); save_cfg(); }
static void ap_distcal(float v){ app_lock(); if (v > 0.8f && v < 1.2f) g_cfg.dist_cal = v; app_unlock(); save_cfg(); }
static void ap_bright(float v) { app_lock(); if (v >= 5 && v <= 100) g_cfg.brightness = (uint8_t)v; app_unlock();
                                 display_set_brightness((int)v); save_cfg(); }

typedef enum {
    MI_FILL, MI_CALIB, MI_PRICE, MI_TANK, MI_RESET_AVG, MI_SHIFT_MODE, MI_LEARN, MI_ECO_UP, MI_SPORT,
    MI_REDLINE, MI_CLEAR_TQ, MI_UNITS, MI_DISTCAL, MI_BRIGHT, MI_PROTO, MI_DEMO, MI_BACK
} menu_item_t;

static void ev_confirm_yes(lv_event_t *e)
{
    logic_cmd_t c = (logic_cmd_t)(intptr_t)lv_event_get_user_data(e);
    logic_post(c, 0);
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target_obj(e)));
    lv_msgbox_close(mbox);
}

static void confirm(const char *msg, logic_cmd_t c)
{
    lv_obj_t *m = lv_msgbox_create(NULL);
    lv_msgbox_add_title(m, "Confirmar");
    lv_msgbox_add_text(m, msg);
    lv_obj_t *yes = lv_msgbox_add_footer_button(m, "Sim");
    lv_obj_add_event_cb(yes, ev_confirm_yes, LV_EVENT_CLICKED, (void *)(intptr_t)c);
    lv_msgbox_add_close_button(m);
}

static void ev_menu(lv_event_t *e)
{
    menu_item_t it = (menu_item_t)(intptr_t)lv_event_get_user_data(e);
    app_lock();
    settings_t c = g_cfg;
    app_unlock();
    switch (it) {
    case MI_FILL: confirm("Tanque cheio agora?\nA autonomia recomeca do zero.", LCMD_FILL_TANK); break;
    case MI_CALIB: open_edit("Litros que couberam ao completar", 0, "%.0f", ap_calib); break;
    case MI_PRICE: open_edit("Preco do litro (R$)", c.fuel_price, "%.2f", ap_price); break;
    case MI_TANK: open_edit("Capacidade do tanque (L)", c.tank_l, "%.0f", ap_tank); break;
    case MI_RESET_AVG: confirm("Zerar a media geral?", LCMD_RESET_AVG); break;
    case MI_SHIFT_MODE:
        app_lock(); g_cfg.shift_mode = (g_cfg.shift_mode + 1) % 3; app_unlock(); save_cfg(); menu_rebuild(); break;
    case MI_LEARN: open_edit("Marcha a aprender (1-6): rode estavel nela", 1, "%.0f", ap_learn); break;
    case MI_ECO_UP: open_edit("ECO: sugerir subir a partir de (rpm)", c.eco_up_rpm, "%.0f", ap_ecoup); break;
    case MI_SPORT: open_edit("SPORT: rpm de troca (sem curva)", c.sport_up_rpm, "%.0f", ap_sport); break;
    case MI_REDLINE: open_edit("Corte de giro (rpm)", c.redline_rpm, "%.0f", ap_redline); break;
    case MI_CLEAR_TQ: confirm("Apagar a curva de torque aprendida?", LCMD_CLEAR_TQ); break;
    case MI_UNITS: app_lock(); g_cfg.units_kml ^= 1; app_unlock(); save_cfg(); menu_rebuild(); break;
    case MI_DISTCAL: open_edit("Fator de distancia (ex.: 1.03)", c.dist_cal, "%.3f", ap_distcal); break;
    case MI_BRIGHT: open_edit("Brilho (5-100 %)", c.brightness, "%.0f", ap_bright); break;
    case MI_PROTO:
        app_lock(); g_cfg.proto = (g_cfg.proto + 1) % 3; app_unlock(); save_cfg();
        obd_post_cmd(CMD_RECONNECT); menu_rebuild(); break;
    case MI_DEMO:
        app_lock(); g_cfg.demo ^= 1; app_unlock(); save_cfg(); obd_post_cmd(CMD_RECONNECT); menu_rebuild(); break;
    case MI_BACK: go(scr_dash); break;
    }
}

static void menu_rebuild(void)
{
    static const char *modes[] = { "Desligado", "Eco", "Sport" };
    static const char *protos[] = { "Automatico", "Renault KWP", "OBD2 padrao" };
    app_lock();
    settings_t c = g_cfg;
    app_unlock();
    int32_t scroll_y = lv_obj_get_scroll_y(menu_list);   // refazer a lista nao pode perder a posicao
    lv_obj_clean(menu_list);
    char b[96];
#define ITEM(id, ...) do { snprintf(b, sizeof b, __VA_ARGS__); \
        lv_obj_t *btn = lv_list_add_button(menu_list, NULL, b); \
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0); \
        lv_obj_add_event_cb(btn, ev_menu, LV_EVENT_CLICKED, (void *)(intptr_t)(id)); } while (0)
    ITEM(MI_BACK, LV_SYMBOL_LEFT "  Voltar ao painel");
    ITEM(MI_FILL, "Abasteci: tanque cheio");
    ITEM(MI_CALIB, "Calibrar consumo (fator atual %.3f)", c.fuel_cal);
    ITEM(MI_PRICE, "Preco do combustivel: R$ %.2f", c.fuel_price);
    ITEM(MI_TANK, "Capacidade do tanque: %.0f L", c.tank_l);
    ITEM(MI_RESET_AVG, "Zerar media geral");
    ITEM(MI_SHIFT_MODE, "Indicador de troca: %s", modes[c.shift_mode % 3]);
    ITEM(MI_LEARN, "Aprender marcha (aprendidas: %d)", (int)(c.gear_ratio[0] > 0) + (c.gear_ratio[1] > 0) +
         (c.gear_ratio[2] > 0) + (c.gear_ratio[3] > 0) + (c.gear_ratio[4] > 0) + (c.gear_ratio[5] > 0));
    ITEM(MI_ECO_UP, "Eco: subir a partir de %.0f rpm", c.eco_up_rpm);
    ITEM(MI_SPORT, "Sport: troca fixa em %.0f rpm", c.sport_up_rpm);
    ITEM(MI_REDLINE, "Corte de giro: %.0f rpm", c.redline_rpm);
    ITEM(MI_CLEAR_TQ, "Apagar curva de torque aprendida");
    ITEM(MI_UNITS, "Unidade: %s", c.units_kml ? "km/L" : "L/100km");
    ITEM(MI_DISTCAL, "Fator de distancia: %.3f", c.dist_cal);
    ITEM(MI_BRIGHT, "Brilho: %d %%", c.brightness);
    ITEM(MI_PROTO, "Protocolo: %s", protos[c.proto % 3]);
    ITEM(MI_DEMO, "Modo demonstracao: %s", c.demo ? "LIGADO" : "desligado");
#undef ITEM
    lv_obj_update_layout(menu_list);
    lv_obj_scroll_to_y(menu_list, scroll_y, LV_ANIM_OFF);
}

static void build_menu(void)
{
    scr_menu = new_screen();
    menu_list = lv_list_create(scr_menu);
    lv_obj_set_size(menu_list, 800, 480);
    lv_obj_set_style_bg_color(menu_list, C_BG, 0);
    lv_obj_set_style_border_width(menu_list, 0, 0);
}

/* ------------------------------------------------------------ falhas */
static void ev_dtc_read(lv_event_t *e) { lv_label_set_text(dtc_msg, "Lendo..."); obd_post_cmd(CMD_READ_DTC); }
static void ev_dtc_clear_yes(lv_event_t *e)
{
    obd_post_cmd(CMD_CLEAR_DTC);
    lv_label_set_text(dtc_msg, "Apagando...");
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target_obj(e)));
    lv_msgbox_close(mbox);
}
static void ev_dtc_clear(lv_event_t *e)
{
    lv_obj_t *m = lv_msgbox_create(NULL);
    lv_msgbox_add_title(m, "Apagar codigos?");
    lv_msgbox_add_text(m, "Faca com o motor DESLIGADO e chave ligada.\n"
                          "Apaga tambem os dados de prontidao (readiness).\n"
                          "Se o defeito existir, o codigo volta.");
    lv_obj_t *yes = lv_msgbox_add_footer_button(m, "Apagar");
    lv_obj_add_event_cb(yes, ev_dtc_clear_yes, LV_EVENT_CLICKED, NULL);
    lv_msgbox_add_close_button(m);
}

static void build_dtc(void)
{
    scr_dtc = new_screen();
    lv_obj_t *t = label(scr_dtc, &lv_font_montserrat_28, C_TEXT, "Codigos de falha (OBD modo 03)");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 16, 10);
    dtc_msg = label(scr_dtc, &lv_font_montserrat_20, C_DIM, "Toque em Ler");
    lv_obj_align(dtc_msg, LV_ALIGN_TOP_LEFT, 16, 50);
    dtc_list = lv_list_create(scr_dtc);
    lv_obj_set_size(dtc_list, 768, 320);
    lv_obj_align(dtc_list, LV_ALIGN_TOP_MID, 0, 85);
    lv_obj_t *b;
    b = button(scr_dtc, "Ler", ev_dtc_read, NULL);       lv_obj_set_size(b, 160, 50); lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 16, -10);
    b = button(scr_dtc, "Apagar", ev_dtc_clear, NULL);   lv_obj_set_size(b, 160, 50); lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -10);
    b = button(scr_dtc, "Voltar", ev_go_dash, NULL);     lv_obj_set_size(b, 160, 50); lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -16, -10);
}

static void update_dtc(void)
{
    app_lock();
    uint32_t seq = g_obd.dtc_seq;
    if (seq == seen_dtc_seq) { app_unlock(); return; }
    seen_dtc_seq = seq;
    int n = g_obd.ndtc;
    char codes[MAX_DTCS][6];
    memcpy(codes, g_obd.dtcs, sizeof codes);
    char msg[64];
    snprintf(msg, sizeof msg, "%s", g_obd.dtc_msg);
    app_unlock();
    lv_label_set_text(dtc_msg, msg);
    lv_obj_clean(dtc_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *it = lv_list_add_text(dtc_list, codes[i]);
        lv_obj_set_style_text_font(it, &lv_font_montserrat_28, 0);
    }
}

/* ------------------------------------------------------------ dados ao vivo */
static void build_data(void)
{
    scr_data = new_screen();
    data_lbl = label(scr_data, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_pos(data_lbl, 16, 10);
    lv_obj_t *b = button(scr_data, "Voltar", ev_go_dash, NULL);
    lv_obj_set_size(b, 160, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -16, -10);
    b = button(scr_data, "Mistura", ev_go_mix, NULL);
    lv_obj_set_size(b, 160, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -192, -10);
    b = button(scr_data, "Eventos", ev_go_ev, NULL);
    lv_obj_set_size(b, 150, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -368, -10);
    b = button(scr_data, "Direcao", ev_go_steer, NULL);
    lv_obj_set_size(b, 150, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -534, -10);
}

static void update_data(const dash_t *d)
{
    app_lock();
    char name[48], status[64];
    snprintf(name, sizeof name, "%s", g_obd.ecu_name);
    snprintf(status, sizeof status, "%s", g_obd.status);
    float rps = g_obd.req_per_s;
    float tq[TQ_BINS];
    memcpy(tq, g_cfg.tq_curve, sizeof tq);
    app_unlock();
    const vehicle_sample_t *s = &d->s;
    char tqs[128] = "";
    int L = 0;
    for (int i = 0; i < TQ_BINS && L < (int)sizeof tqs - 8; i++)
        L += snprintf(tqs + L, sizeof tqs - L, "%.0f ", tq[i]);
    // snprintf da libc, e nao lv_label_set_text_fmt: o printf interno do LVGL esta
    // sem suporte a %f, nao consome o double e o %s seguinte le lixo (trava).
    static char txt[768];
    snprintf(txt, sizeof txt,
        "ECU: %s\n%s   (%.1f leituras/s)\n\n"
        "RPM %.0f   Vel %.0f km/h   Agua %.1f C   Ar %.1f C\n"
        "MAP %.1f kPa   Baro %.1f kPa   Borboleta %.1f %%   Carga %.0f %%\n"
        "T.injecao %.2f ms   Consumo ECU %.2f L/h   Bateria %.2f V\n"
        "Torque ECU %.0f Nm   STFT %.1f %%   LTFT %.1f %%\n\n"
        "Consumo usado: %.2f L/h (fonte %s)\n"
        "Monitor de injecao: %s (desvio %.0f %%)\n"
        "Curva de torque (1000..7500 rpm):\n%s",
        name, status, rps, s->rpm, s->speed_kmh, s->coolant_c, s->iat_c,
        s->map_kpa, s->baro_kpa, s->tps_pct, engine_load01(s) * 100.0f,   // mesma carga da barra
        s->inj_ms, s->fuel_lph_ecu, s->vbat, s->torque_nm, s->stft_pct, s->ltft_pct,
        d->lph, fuel_src_str(d->fuel_src), injmon_str(d->inj_status), d->inj_dev_pct, tqs);
    // dado que a ECU nao manda sai como "nan": troca por "--" (so depois do cabecalho)
    char *p = strstr(txt, "RPM ");
    while (p && (p = strstr(p, "nan")) != NULL) memcpy(p, "-- ", 3);
    lv_label_set_text(data_lbl, txt);
}

/* ------------------------------------------------------------ mistura */
// Grafico 1: tensao da sonda (10 amostras/s, ~15 s). Grafico 2: aquecimento,
// agua e correcao total (curta + longa) desde a partida (1 ponto a cada 5 s, 10 min).
#define MIX_PTS  150
#define WARM_PTS 120

static void ev_go_mix(lv_event_t *e) { obd_set_mix_focus(true); go(scr_mix); }
static void ev_mix_back(lv_event_t *e) { obd_set_mix_focus(false); go(scr_data); }

static lv_obj_t *mix_chart(lv_obj_t *parent, int x, int y, int w, int h, int pts)
{
    lv_obj_t *c = lv_chart_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, pts);
    lv_chart_set_div_line_count(c, 5, 0);
    lv_obj_set_style_bg_color(c, C_PANEL, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_line_color(c, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);     // sem bolinha em cada ponto
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    return c;
}

static void build_mix(void)
{
    scr_mix = new_screen();
    lv_obj_t *t = label(scr_mix, &lv_font_montserrat_20, C_TEXT, "Sonda lambda (0 a 1 V), ultimos 15 s");
    lv_obj_set_pos(t, 16, 8);
    ch_o2 = mix_chart(scr_mix, 16, 36, 500, 190, MIX_PTS);
    lv_chart_set_range(ch_o2, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);            // mV
    ser_o2 = lv_chart_add_series(ch_o2, C_GOOD, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(ch_o2, ser_o2, LV_CHART_POINT_NONE);

    t = label(scr_mix, &lv_font_montserrat_14, C_DIM,
              "Aquecimento desde a partida (10 min): agua 0-120 C (azul), correcao +-30 % (amarelo), malha fechada (verde: em cima = fechada)");
    lv_obj_set_pos(t, 16, 236);
    lv_obj_set_width(t, 500);
    ch_warm = mix_chart(scr_mix, 16, 276, 500, 190, WARM_PTS);
    lv_chart_set_range(ch_warm, LV_CHART_AXIS_PRIMARY_Y, 0, 120);           // graus C
    lv_chart_set_range(ch_warm, LV_CHART_AXIS_SECONDARY_Y, -30, 30);        // %
    ser_temp = lv_chart_add_series(ch_warm, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    ser_trim = lv_chart_add_series(ch_warm, C_WARN, LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_all_value(ch_warm, ser_temp, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(ch_warm, ser_trim, LV_CHART_POINT_NONE);
    ser_loop = lv_chart_add_series(ch_warm, C_GOOD, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(ch_warm, ser_loop, LV_CHART_POINT_NONE);

    mix_lbl = label(scr_mix, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_pos(mix_lbl, 532, 36);
    lv_obj_set_width(mix_lbl, 260);
    lv_obj_set_style_text_line_space(mix_lbl, 8, 0);

    lv_obj_t *b = button(scr_mix, "Voltar", ev_mix_back, NULL);
    lv_obj_set_size(b, 160, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -16, -10);
}

// duty dos bicos: injecao sequencial = 1 injecao a cada 2 voltas do motor
static float inj_duty_pct(const vehicle_sample_t *s)
{
    if (!VALID(s->inj_ms) || !VALID(s->rpm)) return VNAN;
    return s->inj_ms * s->rpm / 1200.0f;
}

// Chamado a cada tick da tela (~30x/s), com qualquer tela aberta, para o
// registro do aquecimento nao depender de a tela "Mistura" estar na frente.
static void mix_sample(const dash_t *d, bool demo)
{
    const vehicle_sample_t *s = &d->s;
    static int div = 0, warm_div = 0;
    static bool was_running = false;
    if (++div % 3) return;                        // 10 amostras/s

    bool live = s->ecu_online && d->engine_running;
    lv_chart_set_next_value(ch_o2, ser_o2, (live && VALID(s->o2_v)) ? (int32_t)(s->o2_v * 1000.0f) : LV_CHART_POINT_NONE);

    if (d->engine_running && !was_running) {      // partida: recomeca o registro
        lv_chart_set_all_value(ch_warm, ser_temp, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ch_warm, ser_trim, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ch_warm, ser_loop, LV_CHART_POINT_NONE);
        lv_chart_set_x_start_point(ch_warm, ser_temp, 0);
        lv_chart_set_x_start_point(ch_warm, ser_trim, 0);
        lv_chart_set_x_start_point(ch_warm, ser_loop, 0);
        warm_n = 0;
        warm_div = 0;
    }
    was_running = d->engine_running;
    int every = demo ? 5 : 50;                    // 5 s por ponto (0,5 s na demonstracao)
    if (live && warm_n < WARM_PTS && ++warm_div >= every) {
        warm_div = 0;
        warm_n++;
        float trim = (VALID(s->stft_pct) ? s->stft_pct : 0) + (VALID(s->ltft_pct) ? s->ltft_pct : 0);
        bool has_trim = VALID(s->stft_pct) || VALID(s->ltft_pct);
        lv_chart_set_next_value(ch_warm, ser_temp, VALID(s->coolant_c) ? (int32_t)s->coolant_c : LV_CHART_POINT_NONE);
        lv_chart_set_next_value(ch_warm, ser_trim, has_trim ? (int32_t)trim : LV_CHART_POINT_NONE);
        lv_chart_set_next_value(ch_warm, ser_loop, VALID(s->loop_closed) ? (s->loop_closed > 0.5f ? 110 : 5) : LV_CHART_POINT_NONE);
    }
}

static void update_mix(const dash_t *d)
{
    const vehicle_sample_t *s = &d->s;
    char o2[16], o2d[16], loop[16], l4[40], l5[40], inj[16], duty[24], temp[16], rpm[16];
    float dc = inj_duty_pct(s);
    fmt_or_dash(o2, sizeof o2, "%.2f V", s->o2_v);
    fmt_or_dash(o2d, sizeof o2d, "%.2f V", s->o2_down_v);
    snprintf(loop, sizeof loop, "%s", !VALID(s->loop_closed) ? "--" : s->loop_closed > 0.5f ? "FECHADA" : "aberta");
    if (s->source == SRC_RENAULT) {       // a ECU Renault nao da trim em %: so o fator bruto e o avanco
        char rich[16], adv[16];
        fmt_or_dash(rich, sizeof rich, "%.0f", s->rich_raw);
        fmt_or_dash(adv, sizeof adv, "%.1f", s->spark_adv);
        snprintf(l4, sizeof l4, "Fator richesse:  %s", rich);
        snprintf(l5, sizeof l5, "Avanco:  %s graus", adv);
    } else {
        char st[16], lt[16];
        fmt_or_dash(st, sizeof st, "%+.1f %%", s->stft_pct);
        fmt_or_dash(lt, sizeof lt, "%+.1f %%", s->ltft_pct);
        snprintf(l4, sizeof l4, "Correcao curta:  %s", st);
        snprintf(l5, sizeof l5, "Correcao longa:  %s", lt);
    }
    fmt_or_dash(inj, sizeof inj, "%.2f ms", s->inj_ms);
    fmt_or_dash(duty, sizeof duty, dc > 85.0f ? "%.0f %% ALTO" : "%.0f %%", dc);
    fmt_or_dash(temp, sizeof temp, "%.0f C", s->coolant_c);
    fmt_or_dash(rpm, sizeof rpm, "%.0f", s->rpm);
    static char txt[512];
    snprintf(txt, sizeof txt,
             "Sonda amont:  %s\nSonda jusante:  %s\nMalha:  %s\n%s\n%s\n"
             "T. injecao:  %s\nDuty dos bicos:  %s\nAgua:  %s\nRPM:  %s",
             o2, o2d, loop, l4, l5, inj, duty, temp, rpm);
    lv_label_set_text(mix_lbl, txt);
    lv_obj_set_style_text_color(mix_lbl, dc > 85.0f ? C_WARN : C_TEXT, 0);
}

/* ------------------------------------------------------------ eventos do A/C */
static void ev_go_ev(lv_event_t *e) { go(scr_ev); }
static void ev_ev_back(lv_event_t *e) { go(scr_data); }

static void build_ev(void)
{
    scr_ev = new_screen();
    lv_obj_t *t = label(scr_ev, &lv_font_montserrat_28, C_TEXT, "Eventos (A/C e direcao)");
    lv_obj_set_pos(t, 16, 8);
    ev_lbl = label(scr_ev, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_pos(ev_lbl, 16, 52);
    lv_obj_set_style_text_line_space(ev_lbl, 4, 0);
    lv_obj_t *b = button(scr_ev, "Voltar", ev_ev_back, NULL);
    lv_obj_set_size(b, 160, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -16, -10);
}

static void update_ev(void)
{
    static uint32_t seen = 0xFFFFFFFFu;
    static char txt[1024];
    app_lock();
    ev_log_t l = g_ev;
    app_unlock();
    if (l.seq == seen) return;
    seen = l.seq;
    int L = 0;
    txt[0] = 0;
    int show = l.n < EV_SHOW ? l.n : EV_SHOW;
    for (int k = 0; k < show && L < (int)sizeof txt - 100; k++) {
        const ev_t *e = &l.e[(l.head - 1 - k + MAX_EVENTS) % MAX_EVENTS];
        char rc[16];
        if (e->rco != 255) snprintf(rc, sizeof rc, "  valv %u%%", e->rco); else rc[0] = 0;
        L += snprintf(txt + L, sizeof txt - L, "%02u:%02u  %s  borb %u%%  %u rpm  %u km/h%s\n",
                      (unsigned)(e->t_s / 60), (unsigned)(e->t_s % 60), g_ev_names[e->type % EV_TYPES],
                      e->tps, e->rpm, e->speed, rc);
    }
    if (!show) snprintf(txt, sizeof txt, "Nenhum evento ainda.\nSo aparece quando a ECU informa o A/C, o pressostato ou a rotacao.");
    lv_label_set_text(ev_lbl, txt);
}

/* ------------------------------------------------------------ direcao e marcha lenta */
// Grafico de ~20 s (10 amostras/s): rpm, valvula de marcha lenta e nivel do pressostato.
#define STEER_PTS 200
static lv_obj_t *scr_steer, *ch_steer, *steer_lbl;
static lv_chart_series_t *ser_s_rpm, *ser_s_rco, *ser_s_ps;

static void ev_go_steer(lv_event_t *e) { obd_set_steer_focus(true); go(scr_steer); }
static void ev_steer_back(lv_event_t *e) { obd_set_steer_focus(false); go(scr_data); }
static void ev_steer_ev(lv_event_t *e) { obd_set_steer_focus(false); go(scr_ev); }

static void build_steer(void)
{
    scr_steer = new_screen();
    lv_obj_t *t = label(scr_steer, &lv_font_montserrat_20, C_TEXT, "Direcao e marcha lenta, ultimos 20 s");
    lv_obj_set_pos(t, 16, 8);
    ch_steer = mix_chart(scr_steer, 16, 36, 500, 300, STEER_PTS);
    lv_chart_set_range(ch_steer, LV_CHART_AXIS_PRIMARY_Y, 0, 2000);          // rpm
    lv_chart_set_range(ch_steer, LV_CHART_AXIS_SECONDARY_Y, 0, 100);         // % da valvula
    ser_s_rpm = lv_chart_add_series(ch_steer, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    ser_s_rco = lv_chart_add_series(ch_steer, C_WARN, LV_CHART_AXIS_SECONDARY_Y);
    ser_s_ps = lv_chart_add_series(ch_steer, C_GOOD, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(ch_steer, ser_s_rpm, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(ch_steer, ser_s_rco, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(ch_steer, ser_s_ps, LV_CHART_POINT_NONE);
    t = label(scr_steer, &lv_font_montserrat_14, C_DIM,
              "rpm 0-2000 (azul), valvula de marcha lenta 0-100 % (amarelo), pressostato (verde: em cima = ativo)");
    lv_obj_set_pos(t, 16, 344);
    lv_obj_set_width(t, 500);
    steer_lbl = label(scr_steer, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_pos(steer_lbl, 532, 36);
    lv_obj_set_width(steer_lbl, 262);
    lv_obj_set_style_text_line_space(steer_lbl, 8, 0);
    lv_obj_t *b = button(scr_steer, "Voltar", ev_steer_back, NULL);
    lv_obj_set_size(b, 150, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -16, -10);
    b = button(scr_steer, "Eventos", ev_steer_ev, NULL);
    lv_obj_set_size(b, 150, 50);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -182, -10);
}

// Chamado a cada tick, com qualquer tela aberta (o grafico nao pode ter buracos).
static void steer_sample(const dash_t *d)
{
    static int div = 0;
    if (++div % 3) return;
    const vehicle_sample_t *s = &d->s;
    bool live = s->ecu_online;
    lv_chart_set_next_value(ch_steer, ser_s_rpm, (live && VALID(s->rpm)) ? (int32_t)s->rpm : LV_CHART_POINT_NONE);
    lv_chart_set_next_value(ch_steer, ser_s_rco, (live && VALID(s->idle_rco)) ? (int32_t)s->idle_rco : LV_CHART_POINT_NONE);
    lv_chart_set_next_value(ch_steer, ser_s_ps, (live && VALID(s->ps_level)) ? (s->ps_level > 0.5f ? 1900 : 50) : LV_CHART_POINT_NONE);
}

static const char *sim_nao(float v) { return !VALID(v) ? "--" : v > 0.5f ? "sim" : "nao"; }

static void update_steer(const dash_t *d)
{
    const vehicle_sample_t *s = &d->s;
    char ps[16], rco[16], rpm[16], set[16], dif[16];
    snprintf(ps, sizeof ps, "%s", !VALID(s->ps_level) ? "--" : s->ps_level > 0.5f ? "ATIVO" : "inativo");
    fmt_or_dash(rco, sizeof rco, "%.0f %%", s->idle_rco);
    fmt_or_dash(rpm, sizeof rpm, "%.0f", s->rpm);
    fmt_or_dash(set, sizeof set, "%.0f", s->idle_set);
    fmt_or_dash(dif, sizeof dif, "%+.0f", (VALID(s->rpm) && VALID(s->idle_set)) ? s->rpm - s->idle_set : NAN);
    static char txt[400];
    snprintf(txt, sizeof txt,
             "Pressostato:  %s\nConectado:  %s\nMarcha lenta ativa:  %s\nMarcha lenta acel.:  %s\n"
             "Valvula:  %s\nRPM:  %s\nConsigna:  %s\nDiferenca:  %s",
             ps, sim_nao(s->ps_conn), sim_nao(s->idle_active), sim_nao(s->idle_acc), rco, rpm, set, dif);
    lv_label_set_text(steer_lbl, txt);
    // vermelho se a rotacao esta bem abaixo da consigna com a regulacao ativa
    bool low = VALID(s->rpm) && VALID(s->idle_set) && s->idle_set > 0 && s->rpm < s->idle_set - 150;
    lv_obj_set_style_text_color(steer_lbl, low ? C_BAD : C_TEXT, 0);
}

/* ------------------------------------------------------------ atualizacao */
static void ui_tick(lv_timer_t *t)
{
    dash_t d;
    app_lock();
    d = g_dash;
    bool kml = g_cfg.units_kml;
    bool sport = g_cfg.shift_mode == SHIFT_SPORT;
    bool demo = g_cfg.demo;
    char status[64];
    snprintf(status, sizeof status, "%s", g_obd.status);
    app_unlock();
    const vehicle_sample_t *s = &d.s;
    char buf[32];

    // ponteiro suavizado: os dados chegam ~5-10x/s, a tela anda a ~30 fps
    float target = (s->ecu_online && VALID(s->rpm)) ? s->rpm : 0;
    disp_rpm += (target - disp_rpm) * 0.35f;
    if (disp_rpm > RPM_MAX) disp_rpm = RPM_MAX;
    lv_scale_set_line_needle_value(scale, needle, NEEDLE_LEN, (int32_t)disp_rpm);

    lv_obj_t *act = lv_screen_active();
    if (act == scr_dash) {
        lv_label_set_text_fmt(lbl_rpm, "%d", (int)(target / 10) * 10);
        fmt_or_dash(buf, sizeof buf, "%.0f", s->ecu_online ? s->speed_kmh : NAN);
        lv_label_set_text(lbl_speed, buf);

        if (d.learning_gear) lv_label_set_text_fmt(lbl_gear, "A%d", d.learn_gear_n);
        else if (d.gear > 0) lv_label_set_text_fmt(lbl_gear, "%d", d.gear);
        else lv_label_set_text(lbl_gear, "N");
        lv_label_set_text(lbl_shift, d.shift == SHIFT_UP ? LV_SYMBOL_UP : d.shift == SHIFT_DOWN ? LV_SYMBOL_DOWN : "");
        lv_obj_set_style_text_color(lbl_shift, d.shift == SHIFT_DOWN ? C_WARN : C_GOOD, 0);

        update_ind(s);

        // modo Sport: botao aceso e, na hora de subir a marcha, pisca a tela (~3x/s)
        static bool sport_shown = false;
        static int blink = 0;
        if (sport != sport_shown) {
            sport_shown = sport;
            lv_obj_set_style_bg_color(btn_sport, sport ? C_BAD : C_PANEL, 0);
        }
        bool flash_on = sport && d.shift == SHIFT_UP && ((blink++ / 5) % 2 == 0);
        if (!(sport && d.shift == SHIFT_UP)) blink = 0;
        if (flash_on == lv_obj_has_flag(shift_flash, LV_OBJ_FLAG_HIDDEN)) {
            if (flash_on) lv_obj_remove_flag(shift_flash, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(shift_flash, LV_OBJ_FLAG_HIDDEN);
        }

        float load = (s->ecu_online && d.engine_running) ? engine_load01(s) : VNAN;
        if (VALID(load)) {
            int pct = (int)(load * 100.0f + 0.5f);
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
            lv_bar_set_value(bar_load, pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_load, pct >= 90 ? C_BAD : pct >= 70 ? C_WARN : C_GOOD, LV_PART_INDICATOR);
            lv_label_set_text_fmt(lbl_load, "%d%%", pct);
        } else {
            lv_bar_set_value(bar_load, 0, LV_ANIM_OFF);
            lv_label_set_text(lbl_load, "--");
        }

        fmt_or_dash(buf, sizeof buf, "%.0f C", s->coolant_c);
        lv_label_set_text(lbl_temp, buf);
        lv_obj_set_style_text_color(lbl_temp, (VALID(s->coolant_c) && s->coolant_c > 105) ? C_BAD : C_TEXT, 0);

        if (isnan(d.inst) && VALID(d.lph)) snprintf(buf, sizeof buf, "%.1f L/h", d.lph);   // parado: L/h
        else fmt_or_dash(buf, sizeof buf, kml ? "%.1f km/L" : "%.1f L/100", d.inst);
        lv_label_set_text(lbl_inst, buf);
        double at = kml ? d.avg_trip : (isnan(d.avg_trip) ? NAN : 100.0 / d.avg_trip);
        double ar = kml ? d.avg_reset : (isnan(d.avg_reset) ? NAN : 100.0 / d.avg_reset);
        fmt_or_dash(buf, sizeof buf, kml ? "%.1f km/L" : "%.1f L/100", at);
        lv_label_set_text(lbl_avg_trip, buf);
        fmt_or_dash(buf, sizeof buf, kml ? "%.1f km/L" : "%.1f L/100", ar);
        lv_label_set_text(lbl_avg_reset, buf);
        snprintf(buf, sizeof buf, "%.0f km", d.range_km);
        lv_label_set_text(lbl_range, buf);
        lv_obj_set_style_text_color(lbl_range, d.range_km < 60 ? C_WARN : C_TEXT, 0);

        lv_label_set_text(lbl_status, status);

        const char *alert = NULL;
        if (VALID(s->coolant_c) && s->coolant_c > 108) alert = "TEMPERATURA DO MOTOR ALTA";
        else if (d.inj_status == INJ_HIGH || d.inj_status == INJ_LOW || d.inj_status == TRIM_HIGH)
            alert = injmon_str(d.inj_status);
        else if (VALID(s->vbat) && s->ecu_online && d.engine_running && s->vbat < 12.4f) alert = "BATERIA / CARGA BAIXA";
        if (alert) {
            lv_label_set_text(lbl_alert, alert);
            lv_obj_remove_flag(lbl_alert, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_alert, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (act == scr_dtc) {
        update_dtc();
    } else if (act == scr_data) {
        static int div = 0;
        if (++div % 6 == 0) update_data(&d);   // ~5x/s basta
    } else if (act == scr_mix) {
        static int div = 0;
        if (++div % 6 == 0) update_mix(&d);
    } else if (act == scr_ev) {
        static int dv = 0;
        if (++dv % 6 == 0) update_ev();
    } else if (act == scr_steer) {
        static int ds = 0;
        if (++ds % 6 == 0) update_steer(&d);
    }
    steer_sample(&d);
    mix_sample(&d, demo);

    if (d.show_report && d.report_seq != seen_report_seq) {
        seen_report_seq = d.report_seq;
        show_report(&d);
    }
}

void ui_init(void)
{
    if (!display_lock()) return;
    build_dash();
    build_menu();
    build_edit();
    build_dtc();
    build_data();
    build_mix();
    build_ev();
    build_steer();
    lv_screen_load(scr_dash);
    app_lock();
    int bright = g_cfg.brightness;
    app_unlock();
    display_set_brightness(bright);    // volta com o brilho salvo (dia/noite)
    lv_timer_create(ui_tick, 33, NULL);
    display_unlock();
}
