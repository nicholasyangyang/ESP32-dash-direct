#include "ui.h"
#include "bsp.h"
#include "lvgl.h"
#include <stdio.h>

LV_FONT_DECLARE(lv_font_speed_120);

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

/* 状态栏: font24(24px) + 上下各5px = 34px → 横线在 y=206 */
#define SPLIT_Y     206

/* ── Widget handles ─────────────────────────────────────────────────────── */
static lv_obj_t *scr_dash    = NULL;
static lv_obj_t *lbl_speed   = NULL;
static lv_obj_t *lbl_gear[4] = {NULL};
static lv_obj_t *lbl_tpms[4] = {NULL};
static lv_obj_t *lbl_icon[7] = {NULL};
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
    if (bar <= 0.0f)               return C_MUTED;
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
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", (int)g_state.speed_kmh);
    lv_label_set_text(lbl_speed, buf);

    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_text_color(lbl_gear[i],
            (i == g_state.gear) ? gear_color(i) : C_DARK_GRAY, 0);
    }

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
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ui_show_dashboard  —  320×240,  横线在黄金分割点 y=148
 *
 *  ┌────────────────────────┬──┬──────────────┐  y=0
 *  │      speed (font48)    │  │   TPMS       │
 *  │         km/h           │  │  FL    FR    │
 *  │    P    R    N    D    │  │  RL    RR    │
 *  ├────────────────────────┴──┴──────────────┤  y=148  ← φ
 *  │                                          │
 *  │      LO  HI  ◄  ►  ⚠  ⚡  AP  (font24)  │
 *  │                                          │
 *  └──────────────────────────────────────────┘  y=240
 *   x=0              x=222 224           x=320
 *
 *  Status bar height = 240 - 148 = 92px
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

    /* ── Speed + Gear panel (x=0, w=222, h=SPLIT_Y) ─────────────────────── */
    lv_obj_t *left = lv_obj_create(scr_dash);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_size(left, 222, SPLIT_Y);
    lv_obj_set_style_bg_color(left, C_BG, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    /* Speed number — 120px font, vertically centered in 0..167 area */
    /* font line_height=89, gear row at y=168 → center_y=(168-89)/2=39 */
    lbl_speed = lv_label_create(left);
    lv_label_set_text(lbl_speed, "0");
    lv_obj_set_style_text_font(lbl_speed, &lv_font_speed_120, 0);
    lv_obj_set_style_text_color(lbl_speed, C_WHITE, 0);
    lv_obj_set_pos(lbl_speed, 0, 39);
    lv_obj_set_width(lbl_speed, 222);
    lv_obj_set_style_text_align(lbl_speed, LV_TEXT_ALIGN_CENTER, 0);

    /* Gear P / R / N / D — font24, evenly spaced */
    for (int i = 0; i < 4; i++) {
        lbl_gear[i] = lv_label_create(left);
        lv_label_set_text(lbl_gear[i], gear_names[i]);
        lv_obj_set_style_text_font(lbl_gear[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl_gear[i], C_DARK_GRAY, 0);
        lv_obj_set_pos(lbl_gear[i], 14 + i * 52, 168);
    }

    /* ── Vertical separator ─────────────────────────────────────────────── */
    lv_obj_t *vsep = lv_obj_create(scr_dash);
    lv_obj_set_pos(vsep, 222, 0);
    lv_obj_set_size(vsep, 2, SPLIT_Y);
    lv_obj_set_style_bg_color(vsep, C_DARK_GRAY, 0);
    lv_obj_set_style_bg_opa(vsep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vsep, 0, 0);
    lv_obj_set_style_pad_all(vsep, 0, 0);

    /* ── TPMS panel (x=224, w=96, h=SPLIT_Y) ───────────────────────────── */
    lv_obj_t *tpms = lv_obj_create(scr_dash);
    lv_obj_set_pos(tpms, 224, 0);
    lv_obj_set_size(tpms, 96, SPLIT_Y);
    lv_obj_set_style_bg_color(tpms, C_BG, 0);
    lv_obj_set_style_bg_opa(tpms, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tpms, 0, 0);
    lv_obj_set_style_pad_all(tpms, 0, 0);
    lv_obj_clear_flag(tpms, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_tpms_hdr = lv_label_create(tpms);
    lv_label_set_text(lbl_tpms_hdr, "TPMS");
    lv_obj_set_style_text_font(lbl_tpms_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_tpms_hdr, C_MUTED, 0);
    lv_obj_set_pos(lbl_tpms_hdr, 0, 4);
    lv_obj_set_width(lbl_tpms_hdr, 96);
    lv_obj_set_style_text_align(lbl_tpms_hdr, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *lbl_tpms_unit = lv_label_create(tpms);
    lv_label_set_text(lbl_tpms_unit, "bar");
    lv_obj_set_style_text_font(lbl_tpms_unit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_tpms_unit, C_DARK_GRAY, 0);
    lv_obj_set_pos(lbl_tpms_unit, 0, 18);
    lv_obj_set_width(lbl_tpms_unit, 96);
    lv_obj_set_style_text_align(lbl_tpms_unit, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *lbl_front = lv_label_create(tpms);
    lv_label_set_text(lbl_front, "Front");
    lv_label_set_long_mode(lbl_front, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lbl_front, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_front, C_DARK_GRAY, 0);
    lv_obj_set_pos(lbl_front, 0, 42);
    lv_obj_set_width(lbl_front, 96);
    lv_obj_set_style_text_align(lbl_front, LV_TEXT_ALIGN_CENTER, 0);

    /* FL=col0 x=6, FR=col1 x=52; RL=col0, RR=col1 */
    static const char *tpms_pos[4]   = {"FL", "FR", "RL", "RR"};
    static const int   tpms_x[4]     = {6, 52, 6, 52};
    static const int   tpms_y_lbl[4] = {56, 56, 130, 130};
    static const int   tpms_y_val[4] = {70, 70, 144, 144};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl_pos = lv_label_create(tpms);
        lv_label_set_text(lbl_pos, tpms_pos[i]);
        lv_label_set_long_mode(lbl_pos, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lbl_pos, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_pos, C_MUTED, 0);
        lv_obj_set_pos(lbl_pos, tpms_x[i], tpms_y_lbl[i]);
        lv_obj_set_width(lbl_pos, 38);

        lbl_tpms[i] = lv_label_create(tpms);
        lv_label_set_text(lbl_tpms[i], "--");
        lv_label_set_long_mode(lbl_tpms[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lbl_tpms[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_tpms[i], C_MUTED, 0);
        lv_obj_set_pos(lbl_tpms[i], tpms_x[i], tpms_y_val[i]);
        lv_obj_set_width(lbl_tpms[i], 38);
    }

    lv_obj_t *lbl_rear = lv_label_create(tpms);
    lv_label_set_text(lbl_rear, "Rear");
    lv_label_set_long_mode(lbl_rear, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lbl_rear, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rear, C_DARK_GRAY, 0);
    lv_obj_set_pos(lbl_rear, 0, 114);
    lv_obj_set_width(lbl_rear, 96);
    lv_obj_set_style_text_align(lbl_rear, LV_TEXT_ALIGN_CENTER, 0);

    /* ── 黄金分割横线 (y=SPLIT_Y) ────────────────────────────────────────── */
    lv_obj_t *hsep = lv_obj_create(scr_dash);
    lv_obj_set_pos(hsep, 0, SPLIT_Y);
    lv_obj_set_size(hsep, 320, 1);
    lv_obj_set_style_bg_color(hsep, C_DARK_GRAY, 0);
    lv_obj_set_style_bg_opa(hsep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hsep, 0, 0);
    lv_obj_set_style_pad_all(hsep, 0, 0);

    /* ── Status bar (y=SPLIT_Y+1, h=240-SPLIT_Y-1=91) ───────────────────── */
    /* 图标用 font24，垂直居中于 91px 区域 */
    lv_obj_t *status = lv_obj_create(scr_dash);
    lv_obj_set_pos(status, 0, SPLIT_Y + 1);
    lv_obj_set_size(status, 320, 240 - SPLIT_Y - 1);
    lv_obj_set_style_bg_color(status, C_BG, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    /* icon_y: center font24 (24px) in 33px → (33-24)/2 = 4 → use 5 */
    for (int i = 0; i < 7; i++) {
        lbl_icon[i] = lv_label_create(status);
        lv_label_set_text(lbl_icon[i], icon_text[i]);
        lv_obj_set_style_text_font(lbl_icon[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl_icon[i], C_DARK_GRAY, 0);
        lv_obj_set_pos(lbl_icon[i], (int)(320 * i / 7 + 320 / 14 - 10), 5);
    }

    lv_scr_load(scr_dash);
    timer_refresh = lv_timer_create(ui_refresh, 16, NULL);

    lvgl_port_unlock();
}
