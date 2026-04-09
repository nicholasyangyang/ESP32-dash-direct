#pragma once
#include <stdbool.h>
#include <stdint.h>

// ── HW4 CAN IDs decoded by this module ───────────────────────────────────
// Source: joshwardell/model3dbc + tesla-can-explorer.gapinski.eu (fw 2026.2)
//
// 0x257  DI_speed                speed_kmh
// 0x118  DI_systemStatus         gear (0=P 1=R 2=N 3=D, 0xFF=invalid)
// 0x33A  UI_range                range_km  (UI_Range 0|10@1+)
//                                soc_pct   (UI_SOC   48|7@1+)  ← primary SOC
// 0x292  BMS_socStatus           soc_pct   (SOCUI292 10|10@1+ (0.1,0)) ← backup SOC
// 0x252  BMS_powerAvailable      regen_kw  (BMS_maxRegenPower 0|16@1+ (0.01,0) kW)
// 0x212  BMS_status              charging  (BMS_chargeRequest 29|1)
// 0x528  VCFRONT_hvac            temp_outside_c
// 0x312  BMS_packStatus          batt_max_temp_c
// 0x334  UI_powertrainControl    regen_pct (UI_regenTorqueMax 24|8@1+ (0.5,0) %)
// 0x3F5  VCFRONT_vehicleLights   low_beam/high_beam/hazard/turn
// 0x399  DAS_status              ap_engaged
// 0x219  VCSEC_TPMSData          tpms_fl/fr/rl/rr (bar)

typedef struct {
    float   speed_kmh;
    uint8_t gear;             // 0=P 1=R 2=N 3=D
    float   range_km;
    float   soc_pct;
    bool    charging;
    float   temp_outside_c;
    float   batt_max_temp_c;
    float   regen_kw;              // max regen power available (kW) — BMS_powerAvailable 0x252
    float   regen_pct;             // UI regen torque limit setting (%) — UI_powertrainControl 0x334
    bool    low_beam;
    bool    high_beam;
    bool    turn_left;
    bool    turn_right;
    bool    hazard;
    bool    ap_engaged;
    float   tpms_fl;          // bar, 0=unknown
    float   tpms_fr;
    float   tpms_rl;
    float   tpms_rr;
    bool    can_ok;           // TWAI bus running & receiving frames
} dashboard_state_t;

/** Global state: written by can_rx_task, read by LVGL ui_refresh timer. */
extern dashboard_state_t g_state;

/** Install TWAI driver on IO10(TX)/IO11(RX) at 500kbps and spawn rx task. */
void can_direct_start(void);
