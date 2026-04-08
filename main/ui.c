#include "ui.h"
#include "bsp.h"
#include "lvgl.h"
#include <stdio.h>

/* ── Color palette ─────────────────────────────────────────────────────── */
#define C_BG        lv_color_hex(0x0d0d0d)
#define C_WHITE     lv_color_hex(0xffffff)
#define C_GREEN     lv_color_hex(0x3fb950)
#define C_ORANGE    lv_color_hex(0xf0883e)
#define C_YELLOW    lv_color_hex(0xe3b341)
#define C_RED       lv_color_hex(0xf85149)
#define C_DARK_GRAY lv_color_hex(0x30363d)
#define C_BLUE      lv_color_hex(0x388bfd)
#define C_MUTED     lv_color_hex(0x8b949e)

/* ── Widget handles ─────────────────────────────────────────────────────── */
static lv_obj_t *scr_dash   = NULL;

/* Left panel */
static lv_obj_t *lbl_speed  = NULL;
static lv_obj_t *lbl_gear[4]= {NULL};

static lv_obj_t *bar_soc    = NULL;
static lv_obj_t *lbl_soc    = NULL;

/* Right panel */
static lv_obj_t *lbl_range_val    = NULL;
static lv_obj_t *lbl_temp_out_val = NULL;
static lv_obj_t *lbl_batt_val     = NULL;
static lv_obj_t *bar_regen        = NULL;

/* TPMS panel — order: FL, FR, RL, RR */
static lv_obj_t *lbl_tpms[4] = {NULL};

/* Status bar */
static lv_obj_t *lbl_icon[7] = {NULL};

/* Timer */
static lv_timer_t *timer_refresh = NULL;

/* ── Gear config ────────────────────────────────────────────────────────── */
static const char *gear_names[4] = {"P", "R", "N", "D"};

static lv_color_t gear_color(uint8_t g)
{
    switch (g) {
        case 0: return C_MUTED;
        case 1: return C_ORANGE;
        case 2: return C_YELLOW;
        case 3: return C_GREEN;
        default: return C_DARK_GRAY;
    }
}

/* ── Status icon config ─────────────────────────────────────────────────── */
static const char *icon_text[7] = {
    "LO", "HI", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT,
    LV_SYMBOL_WARNING, LV_SYMBOL_CHARGE, "AP"
};

static lv_color_t icon_active_color(int idx)
{
    switch (idx) {
        case 0: return C_WHITE;
        case 1: return C_WHITE;
        case 2: return C_YELLOW;
        case 3: return C_YELLOW;
        case 4: return C_RED;
        case 5: return C_GREEN;
        case 6: return C_BLUE;
        default: return C_WHITE;
    }
}

static bool icon_active(int idx)
{
    switch (idx) {
        case 0: return g_state.low_beam;
        case 1: return g_state.high_beam;
        case 2: return g_state.turn_left;
        case 3: return g_state.turn_right;
        case 4: return g_state.hazard;
        case 5: return g_state.charging;
        case 6: return g_state.ap_engaged;
        default: return false;
    }
}

static lv_color_t tpms_color(float bar)
{
    if (bar <= 0.0f)            return C_MUTED;   // no data yet
    if (bar < 2.0f || bar > 3.5f) return C_RED;
    if (bar < 2.5f || bar > 3.1f) return C_ORANGE;
    return C_GREEN;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ui_refresh  – LVGL timer, 60 Hz
 * ══════════════════════════════════════════════════════════════════════════ */
static void ui_refresh(lv_timer_t *t)
{
    (void)t;
    char buf[32];

    snprintf(buf, sizeof(buf), "%d", (int)g_state.speed_kmh);
    lv_label_set_text(lbl_speed, buf);

    for (int i = 0; i < 4; i++) {
        if (i == g_state.gear) {
            lv_obj_set_style_text_color(lbl_gear[i], gear_color(i), 0);
            lv_obj_set_style_text_font(lbl_gear[i], &lv_font_montserrat_24, 0);
        } else {
            lv_obj_set_style_text_color(lbl_gear[i], C_DARK_GRAY, 0);
            lv_obj_set_style_text_font(lbl_gear[i], &lv_font_montserrat_16, 0);
        }
    }

    lv_bar_set_value(bar_soc, (int)g_state.soc_pct, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d%%", (int)g_state.soc_pct);
    lv_label_set_text(lbl_soc, buf);

    snprintf(buf, sizeof(buf), "%d km", (int)g_state.range_km);
    lv_label_set_text(lbl_range_val, buf);

    snprintf(buf, sizeof(buf), "%.0f°C", g_state.temp_outside_c);
    lv_label_set_text(lbl_temp_out_val, buf);

    snprintf(buf, sizeof(buf), "%.0f°C", g_state.batt_max_temp_c);
    lv_label_set_text(lbl_batt_val, buf);

    lv_bar_set_value(bar_regen, (int)g_state.regen_pct, LV_ANIM_OFF);

    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_text_color(lbl_icon[i],
            icon_active(i) ? icon_active_color(i) : C_DARK_GRAY, 0);
    }

    float tpms_vals[4] = {
        g_state.tpms_fl, g_state.tpms_fr,
        g_state.tpms_rl, g_state.tpms_rr
    };
    for (int i = 0; i < 4; i++) {
        if (tpms_vals[i] > 0.0f)
            snprintf(buf, sizeof(buf), "%.1f", tpms_vals[i]);
        else
            snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(lbl_tpms[i], buf);
        lv_obj_set_style_text_color(lbl_tpms[i], tpms_color(tpms_vals[i]), 0);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ui_init
 * ══════════════════════════════════════════════════════════════════════════ */
void ui_init(void)
{
    /* screens built lazily in ui_show_dashboard */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ui_show_dashboard
 * ══════════════════════════════════════════════════════════════════════════ */
void ui_show_dashboard(void)
{
    if (!lvgl_port_lock(0)) return;

    if (scr_dash != NULL) {
        lv_scr_load(scr_dash);
        if (timer_refresh == NULL)
            timer_refresh = lv_timer_create(ui_refresh, 16, NULL);
        lvgl_port_unlock();
        return;
    }

    scr_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_dash, C_BG, 0);
    lv_obj_set_style_bg_opa(scr_dash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr_dash, 0, 0);
    lv_obj_set_style_pad_all(scr_dash, 0, 0);

    /* ── Left panel (x=0, w=180, h=210) ─────────────────────────────────── */
    lv_obj_t *left = lv_obj_create(scr_dash);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_size(left, 180, 210);
    lv_obj_set_style_bg_color(left, C_BG, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    lbl_speed = lv_label_create(left);
    lv_label_set_text(lbl_speed, "0");
    lv_obj_set_style_text_font(lbl_speed, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_speed, C_WHITE, 0);
    lv_obj_set_pos(lbl_speed, 0, 20);
    lv_obj_set_width(lbl_speed, 180);
    lv_obj_set_style_text_align(lbl_speed, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *lbl_unit = lv_label_create(left);
    lv_label_set_text(lbl_unit, "km/h");
    lv_obj_set_style_text_font(lbl_unit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_unit, C_MUTED, 0);
    lv_obj_set_pos(lbl_unit, 130, 82);

    for (int i = 0; i < 4; i++) {
        lbl_gear[i] = lv_label_create(left);
        lv_label_set_text(lbl_gear[i], gear_names[i]);
        lv_obj_set_style_text_font(lbl_gear[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_gear[i], C_DARK_GRAY, 0);
        lv_obj_set_pos(lbl_gear[i], 22 + i * 45 - 8, 132);
    }

    lbl_soc = lv_label_create(left);
    lv_label_set_text(lbl_soc, "0%");
    lv_obj_set_style_text_font(lbl_soc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_soc, C_GREEN, 0);
    lv_obj_set_pos(lbl_soc, 0, 170);
    lv_obj_set_width(lbl_soc, 172);
    lv_obj_set_style_text_align(lbl_soc, LV_TEXT_ALIGN_RIGHT, 0);

    bar_soc = lv_bar_create(left);
    lv_obj_set_pos(bar_soc, 8, 186);
    lv_obj_set_size(bar_soc, 164, 14);
    lv_bar_set_range(bar_soc, 0, 100);
    lv_bar_set_value(bar_soc, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_soc, C_DARK_GRAY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_soc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_soc, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_soc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_soc, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_soc, 4, LV_PART_INDICATOR);

    /* ── Right panel (x=182, w=76, h=210) ───────────────────────────────── */
    lv_obj_t *right = lv_obj_create(scr_dash);
    lv_obj_set_pos(right, 182, 0);
    lv_obj_set_size(right, 76, 210);
    lv_obj_set_style_bg_color(right, C_BG, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_range_key = lv_label_create(right);
    lv_label_set_text(lbl_range_key, "Range");
    lv_obj_set_style_text_font(lbl_range_key, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_range_key, C_MUTED, 0);
    lv_obj_set_pos(lbl_range_key, 4, 4);

    lbl_range_val = lv_label_create(right);
    lv_label_set_text(lbl_range_val, "--- km");
    lv_obj_set_style_text_font(lbl_range_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_range_val, C_WHITE, 0);
    lv_obj_set_pos(lbl_range_val, 4, 20);

    lv_obj_t *lbl_temp_out_key = lv_label_create(right);
    lv_label_set_text(lbl_temp_out_key, "OutTemp");
    lv_obj_set_style_text_font(lbl_temp_out_key, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_temp_out_key, C_MUTED, 0);
    lv_obj_set_pos(lbl_temp_out_key, 4, 56);

    lbl_temp_out_val = lv_label_create(right);
    lv_label_set_text(lbl_temp_out_val, "--°C");
    lv_obj_set_style_text_font(lbl_temp_out_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_temp_out_val, C_WHITE, 0);
    lv_obj_set_pos(lbl_temp_out_val, 4, 72);

    lv_obj_t *lbl_batt_key = lv_label_create(right);
    lv_label_set_text(lbl_batt_key, "BattTemp");
    lv_obj_set_style_text_font(lbl_batt_key, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_batt_key, C_MUTED, 0);
    lv_obj_set_pos(lbl_batt_key, 4, 108);

    lbl_batt_val = lv_label_create(right);
    lv_label_set_text(lbl_batt_val, "--°C");
    lv_obj_set_style_text_font(lbl_batt_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_batt_val, C_WHITE, 0);
    lv_obj_set_pos(lbl_batt_val, 4, 124);

    lv_obj_t *lbl_regen_key = lv_label_create(right);
    lv_label_set_text(lbl_regen_key, LV_SYMBOL_LEFT " Regen");
    lv_obj_set_style_text_font(lbl_regen_key, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_regen_key, C_MUTED, 0);
    lv_obj_set_pos(lbl_regen_key, 4, 160);

    bar_regen = lv_bar_create(right);
    lv_obj_set_pos(bar_regen, 4, 178);
    lv_obj_set_size(bar_regen, 64, 14);
    lv_bar_set_range(bar_regen, 0, 100);
    lv_bar_set_value(bar_regen, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_regen, C_DARK_GRAY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_regen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_regen, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_regen, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_regen, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_regen, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar_regen, C_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar_regen, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    /* ── TPMS vertical strip (x=260, w=60, h=210) ───────────────────────── */
    lv_obj_t *tpms_sep = lv_obj_create(scr_dash);
    lv_obj_set_pos(tpms_sep, 258, 0);
    lv_obj_set_size(tpms_sep, 2, 210);
    lv_obj_set_style_bg_color(tpms_sep, C_DARK_GRAY, 0);
    lv_obj_set_style_bg_opa(tpms_sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tpms_sep, 0, 0);
    lv_obj_set_style_pad_all(tpms_sep, 0, 0);

    lv_obj_t *tpms = lv_obj_create(scr_dash);
    lv_obj_set_pos(tpms, 260, 0);
    lv_obj_set_size(tpms, 60, 210);
    lv_obj_set_style_bg_color(tpms, C_BG, 0);
    lv_obj_set_style_bg_opa(tpms, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tpms, 0, 0);
    lv_obj_set_style_pad_all(tpms, 0, 0);
    lv_obj_clear_flag(tpms, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_tpms_hdr = lv_label_create(tpms);
    lv_label_set_text(lbl_tpms_hdr, "TPMS");
    lv_obj_set_style_text_font(lbl_tpms_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_tpms_hdr, C_MUTED, 0);
    lv_obj_set_pos(lbl_tpms_hdr, 0, 6);
    lv_obj_set_width(lbl_tpms_hdr, 60);
    lv_obj_set_style_text_align(lbl_tpms_hdr, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *lbl_tpms_unit = lv_label_create(tpms);
    lv_label_set_text(lbl_tpms_unit, "bar");
    lv_obj_set_style_text_font(lbl_tpms_unit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_tpms_unit, C_DARK_GRAY, 0);
    lv_obj_set_pos(lbl_tpms_unit, 0, 22);
    lv_obj_set_width(lbl_tpms_unit, 60);
    lv_obj_set_style_text_align(lbl_tpms_unit, LV_TEXT_ALIGN_CENTER, 0);

    /* Front axle label */
    lv_obj_t *lbl_front = lv_label_create(tpms);
    lv_label_set_text(lbl_front, "-- FRONT --");
    lv_obj_set_style_text_font(lbl_front, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_front, C_DARK_GRAY, 0);
    lv_obj_set_pos(lbl_front, 0, 44);
    lv_obj_set_width(lbl_front, 60);
    lv_obj_set_style_text_align(lbl_front, LV_TEXT_ALIGN_CENTER, 0);

    /* FL / FR position labels */
    static const char *tpms_pos[4] = {"FL", "FR", "RL", "RR"};
    static const int   tpms_x[4]   = {2, 32, 2, 32};
    static const int   tpms_y_lbl[4] = {60, 60, 130, 130};
    static const int   tpms_y_val[4] = {74, 74, 144, 144};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl_pos = lv_label_create(tpms);
        lv_label_set_text(lbl_pos, tpms_pos[i]);
        lv_obj_set_style_text_font(lbl_pos, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_pos, C_MUTED, 0);
        lv_obj_set_pos(lbl_pos, tpms_x[i], tpms_y_lbl[i]);

        lbl_tpms[i] = lv_label_create(tpms);
        lv_label_set_text(lbl_tpms[i], "--");
        lv_obj_set_style_text_font(lbl_tpms[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_tpms[i], C_MUTED, 0);
        lv_obj_set_pos(lbl_tpms[i], tpms_x[i], tpms_y_val[i]);
    }

    /* Rear axle label */
    lv_obj_t *lbl_rear = lv_label_create(tpms);
    lv_label_set_text(lbl_rear, "-- REAR --");
    lv_obj_set_style_text_font(lbl_rear, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rear, C_DARK_GRAY, 0);
    lv_obj_set_pos(lbl_rear, 0, 114);
    lv_obj_set_width(lbl_rear, 60);
    lv_obj_set_style_text_align(lbl_rear, LV_TEXT_ALIGN_CENTER, 0);

    /* ── Status bar (y=210, h=30) ────────────────────────────────────────── */
    lv_obj_t *sep = lv_obj_create(scr_dash);
    lv_obj_set_pos(sep, 0, 210);
    lv_obj_set_size(sep, 320, 1);
    lv_obj_set_style_bg_color(sep, C_DARK_GRAY, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);

    lv_obj_t *status = lv_obj_create(scr_dash);
    lv_obj_set_pos(status, 0, 211);
    lv_obj_set_size(status, 320, 29);
    lv_obj_set_style_bg_color(status, C_BG, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 7; i++) {
        lbl_icon[i] = lv_label_create(status);
        lv_label_set_text(lbl_icon[i], icon_text[i]);
        lv_obj_set_style_text_font(lbl_icon[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_icon[i], C_DARK_GRAY, 0);
        lv_obj_set_pos(lbl_icon[i], (int)(320 * i / 7 + 320 / 14 - 8), 5);
    }

    lv_scr_load(scr_dash);
    timer_refresh = lv_timer_create(ui_refresh, 16, NULL);

    lvgl_port_unlock();
}
