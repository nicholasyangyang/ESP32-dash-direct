#include "can_direct.h"
#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG         "can_direct"
#define CAN_TX_PIN  GPIO_NUM_10
#define CAN_RX_PIN  GPIO_NUM_11

dashboard_state_t g_state = {0};

// ── Frame parsing — HW4, aligned with model3dbc/Model3CAN.dbc ────────────
//
// 0x257  DI_speedChecksum   :  0| 8@1+  (1,0)
//        DI_speedCounter    :  8| 4@1+  (1,0)
//        DI_vehicleSpeed    : 12|12@1+  (0.08,-40)  kph  ← precise speed
//        DI_uiSpeedUnits    : 33| 1@1+  (1,0)        0=mph 1=kph
//        DI_uiSpeed         : 24| 9@1+  (1,0)        UI display integer
//        DI_uiSpeedHighSpeed: 34| 9@1+  (1,0)
//
// 0x118  DI_systemStatusChecksum : 0|8@1+
//        DI_systemStatusCounter  : 8|4@1+
//        DI_gear                 :21|3@1+  (1,0)  0=P 1=R 2=N 3=D
//
// 0x252  DI_state (decimal 594):
//        DI_uiRange :  0|12@1+ (0.1,0) "km or miles"
//

// 0x292  BMS_SOC (decimal 658 in DBC):
//        SOCmin292  :  0|10@1+  (0.1,0) "%"
//        SOCUI292   : 10|10@1+  (0.1,0) "%"   ← display SOC
//        SOCave292  : 30|10@1+  (0.1,0) "%"
//
// 0x212  BMS_status (decimal 530):
//        BMS_chargeRequest : 29|1@1+           ← charging flag
//
// 0x528  UnixTime in DBC — but community-verified hvac:
//        byte[2] = outside temp (int8, °C)
//
// 0x312  BMSthermal (decimal 786):
//        BMSmaxPackTemperature : 53|9@1+ (0.25,-25) "C"
//
// 0x334  UI_powertrainControl (decimal 820):
//        UI_regenTorqueMax : 24|8@1+ (0.5,0) "%"   ← byte[3]
//
// 0x3F5  VCFRONT_lighting (decimal 1013):
//        VCFRONT_lowBeamLeftStatus  : 28|2@1+
//        VCFRONT_lowBeamRightStatus : 30|2@1+
//        VCFRONT_highBeamLeftStatus : 32|2@1+
//        VCFRONT_highBeamRightStatus: 34|2@1+
//        VCFRONT_hazardLightRequest :  4|4@1+  (!=0 → hazard)
//        VCFRONT_indicatorLeftRequest : 0|2@1+ (1=on)
//        VCFRONT_indicatorRightRequest: 2|2@1+ (1=on)
//
// 0x399  DAS_status (decimal 921):
//        DAS_autopilotState : 0|4@1+  3/4/5 = engaged

static void parse_frame(uint32_t id, uint8_t dlc, const uint8_t *d)
{
    switch (id) {

    // ── 车速 ─────────────────────────────────────────────────────────────
    case 0x257:
        // DI_uiSpeed      : 24|9@1+ (1,0)          → UI 显示速度整数（mph 或 kph）
        // DI_uiSpeedUnits : 33|1@1+ 0=mph 1=kph
        // DI_vehicleSpeed : 12|12@1+ (0.08,-40) kph → 精确速度
        if (dlc >= 5) {
            uint16_t raw_ui = (uint16_t)d[3] | ((uint16_t)(d[4] & 0x01) << 8);
            uint8_t  units  = (d[4] >> 1) & 0x01;   // bit33: 0=mph 1=kph
            // 精确速度（始终 kph）
            uint16_t raw_v  = ((uint16_t)d[1] >> 4) | ((uint16_t)d[2] << 4);
            float    v_kph  = raw_v * 0.08f - 40.0f;
            if (v_kph < 0.0f) v_kph = 0.0f;
            // 仪表如设 mph 则 uiSpeed 是 mph 值，换算成 kph 显示
            g_state.speed_kmh = (units == 0) ? (raw_ui * 1.60934f) : (float)raw_ui;
            ESP_LOGD(TAG, "0x257 uiSpeed=%u units=%s v_kph=%.1f → show=%.0f  [%02X %02X %02X %02X %02X]",
                     raw_ui, units ? "kph" : "mph", v_kph, g_state.speed_kmh,
                     d[0], d[1], d[2], d[3], d[4]);
        }
        break;

    // ── 档位 ─────────────────────────────────────────────────────────────
    case 0x118:
        // DI_gear : 21|3@1+ → bit21 = byte2.bit5
        // DBC: 0=SNA 1=P 2=R 3=N 4=D  →  存为 0-based: 0=P 1=R 2=N 3=D, 0xFF=无效
        if (dlc >= 3) {
            uint8_t raw_gear = (d[2] >> 5) & 0x07;
            g_state.gear = (raw_gear >= 1 && raw_gear <= 4) ? (raw_gear - 1) : 0xFF;
            ESP_LOGD(TAG, "0x118 gear raw=%u → gear=%u  [%02X %02X %02X]",
                     raw_gear, g_state.gear, d[0], d[1], d[2]);
        }
        break;

    // ── 续航里程 + SOC UI 显示值 ──────────────────────────────────────────
    // 0x33A  ID33AUI_rangeSOC (decimal 826):
    //   UI_Range      :  0|10@1+ (1,0)  → 仪表显示里程（与车辆单位一致 km/mi）
    //   UI_idealRange : 16|10@1+ (1,0)  → 理想里程
    //   UI_SOC        : 48| 7@1+ (1,0) "%"  → 显示电量整数 0-100
    case 0x33A:
        if (dlc >= 7) {
            uint16_t raw_range = (uint16_t)d[0] | ((uint16_t)(d[1] & 0x03) << 8);
            uint8_t  raw_soc   = d[6] & 0x7F;
            if (raw_range > 0)
                g_state.range_km = (float)raw_range;
            if (raw_soc > 0)
                g_state.soc_pct = (float)raw_soc;
            ESP_LOGI(TAG, "0x33A range=%u(%.0f) soc=%u%%  [%02X %02X .. %02X %02X]",
                     raw_range, g_state.range_km, raw_soc,
                     d[0], d[1], d[6], d[7]);
        }
        break;

    // ── 电池 SOC (备用: 0x292 BMS_SOC) ───────────────────────────────────
    // SOCUI292 : 10|10@1+ (0.1,0) — HW4 可能不广播此帧
    case 0x292:
        if (dlc >= 3) {
            uint16_t raw_ui = (uint16_t)((d[1] >> 2) & 0x3F)
                            | ((uint16_t)(d[2] & 0x0F) << 6);
            if (raw_ui > 0 && g_state.soc_pct == 0.0f)
                g_state.soc_pct = raw_ui * 0.1f;
            ESP_LOGI(TAG, "0x292 SOCui_raw=%u → %.1f%%  [%02X %02X %02X]",
                     raw_ui, raw_ui * 0.1f, d[0], d[1], d[2]);
        }
        break;

    // ── 续航里程备用: 0x252 DI_state ──────────────────────────────────────
    case 0x252:
        if (dlc >= 2) {
            uint16_t raw = (uint16_t)d[0] | ((uint16_t)(d[1] & 0x0F) << 8);
            if (raw > 0 && g_state.range_km == 0.0f)
                g_state.range_km = raw * 0.1f;
            ESP_LOGI(TAG, "0x252 range_raw=%u → %.1f  [%02X %02X]", raw, raw * 0.1f, d[0], d[1]);
        }
        break;

    // ── 充电状态 ──────────────────────────────────────────────────────────
    case 0x212:
        // BMS_chargeRequest : 29|1 → bit29 = byte3.bit5
        if (dlc >= 4)
            g_state.charging = (d[3] >> 5) & 0x01;
        break;

    // ── 车外温度 ──────────────────────────────────────────────────────────
    case 0x528:
        // 社区验证：byte[2] = 外部温度 int8 °C
        if (dlc >= 3)
            g_state.temp_outside_c = (float)(int8_t)d[2];
        break;

    // ── 电池最高温度 ──────────────────────────────────────────────────────
    case 0x312:
        // BMSmaxPackTemperature : 53|9@1+ (0.25,-25) "C"
        // bit53 = byte6.bit5 → 3位来自byte6[7:5], 6位来自byte7[5:0]
        if (dlc >= 8) {
            uint16_t raw = (uint16_t)((d[6] >> 5) & 0x07)
                         | ((uint16_t)(d[7] & 0x3F) << 3);
            g_state.batt_max_temp_c = raw * 0.25f - 25.0f;
        }
        break;

    // ── 回收强度 ──────────────────────────────────────────────────────────
    case 0x334:
        // UI_regenTorqueMax : 24|8@1+ (0.5,0) "%" → byte[3]
        if (dlc >= 4)
            g_state.regen_pct = d[3] * 0.5f;
        break;

    // ── 灯光状态 ──────────────────────────────────────────────────────────
    case 0x3F5:
        // VCFRONT_indicatorLeftRequest  :  0|2@1+  byte0 bits[1:0]
        // VCFRONT_indicatorRightRequest :  2|2@1+  byte0 bits[3:2]
        // VCFRONT_hazardLightRequest    :  4|4@1+  byte0 bits[7:4]
        // VCFRONT_lowBeamLeftStatus     : 28|2@1+  byte3 bits[5:4]
        // VCFRONT_lowBeamRightStatus    : 30|2@1+  byte3 bits[7:6]
        // VCFRONT_highBeamLeftStatus    : 32|2@1+  byte4 bits[1:0]
        // VCFRONT_highBeamRightStatus   : 34|2@1+  byte4 bits[3:2]
        if (dlc >= 7) {
            uint8_t ind_l  = (d[0] >> 0) & 0x03;   // bit 0|2
            uint8_t ind_r  = (d[0] >> 2) & 0x03;   // bit 2|2
            uint8_t haz    = (d[0] >> 4) & 0x0F;   // bit 4|4
            uint8_t lo_l   = (d[3] >> 4) & 0x03;   // bit 28|2
            uint8_t lo_r   = (d[3] >> 6) & 0x03;   // bit 30|2
            uint8_t hi_l   = (d[4] >> 0) & 0x03;   // bit 32|2
            uint8_t hi_r   = (d[4] >> 2) & 0x03;   // bit 34|2
            g_state.turn_left  = ind_l == 1;
            g_state.turn_right = ind_r == 1;
            g_state.hazard     = haz != 0;
            g_state.low_beam   = lo_l == 1 || lo_r == 1;
            g_state.high_beam  = hi_l == 1 || hi_r == 1;
            ESP_LOGD(TAG, "0x3F5 indL=%u indR=%u haz=%u loL=%u loR=%u hiL=%u hiR=%u  [%02X %02X %02X %02X %02X]",
                     ind_l, ind_r, haz, lo_l, lo_r, hi_l, hi_r,
                     d[0], d[1], d[2], d[3], d[4]);
        }
        break;

    // ── 胎压 ─────────────────────────────────────────────────────────────
    case 0x219:
        // VCSEC_TPMSData: byte[0]=index(0=FL,1=FR,2=RL,3=RR), byte[1]=raw*0.025 bar
        if (dlc >= 2) {
            float p = d[1] * 0.025f;
            switch (d[0] & 0x03) {
                case 0: g_state.tpms_fl = p; break;
                case 1: g_state.tpms_fr = p; break;
                case 2: g_state.tpms_rl = p; break;
                case 3: g_state.tpms_rr = p; break;
            }
        }
        break;

    // ── Autopilot 状态 ────────────────────────────────────────────────────
    case 0x399:
        // DAS_autopilotState : 0|4@1+ → byte0 bits[3:0]
        // 3=ACTIVE_NOMINAL 4=RESTRICTED 5=NAV_AP
        if (dlc >= 1) {
            uint8_t ap = d[0] & 0x0F;
            g_state.ap_engaged = (ap == 3 || ap == 4 || ap == 5);
        }
        break;

    default:
        break;
    }
}

// ── TWAI 安装+启动（也用于bus-off恢复）──────────────────────────────────
static esp_err_t twai_install_and_start(void)
{
    twai_general_config_t gc = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    gc.rx_queue_len = 32;
    twai_timing_config_t tc = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t fc = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    esp_err_t r = twai_driver_install(&gc, &tc, &fc);
    if (r != ESP_OK) return r;
    return twai_start();
}

// ── RX 任务 ──────────────────────────────────────────────────────────────
static void can_rx_task(void *arg)
{
    (void)arg;
    uint32_t last_rx_tick  = 0;
    uint32_t last_stat_tick = 0;
    uint32_t cnt_total = 0, cnt_33a = 0, cnt_292 = 0, cnt_252 = 0, cnt_257 = 0, cnt_118 = 0;

    for (;;) {
        twai_message_t msg;
        if (twai_receive(&msg, pdMS_TO_TICKS(200)) == ESP_OK) {
            cnt_total++;
            if (msg.identifier == 0x33A) cnt_33a++;
            if (msg.identifier == 0x292) cnt_292++;
            if (msg.identifier == 0x252) cnt_252++;
            if (msg.identifier == 0x257) cnt_257++;
            if (msg.identifier == 0x118) cnt_118++;
            parse_frame(msg.identifier, msg.data_length_code, msg.data);
            last_rx_tick   = xTaskGetTickCount();
            g_state.can_ok = true;

            // 每 5 秒打印一次统计
            uint32_t now = xTaskGetTickCount();
            if ((now - last_stat_tick) >= pdMS_TO_TICKS(5000)) {
                last_stat_tick = now;
                ESP_LOGI(TAG, "CAN stats: total=%lu  0x257=%lu  0x118=%lu"
                              "  0x33A(range+SOC)=%lu  0x292(SOC bak)=%lu  0x252(range bak)=%lu"
                              "  → SOC=%.0f%%  range=%.0f",
                         cnt_total, cnt_257, cnt_118, cnt_33a, cnt_292, cnt_252,
                         g_state.soc_pct, g_state.range_km);
                cnt_total = cnt_33a = cnt_292 = cnt_252 = cnt_257 = cnt_118 = 0;
            }
        } else {
            if ((xTaskGetTickCount() - last_rx_tick) > pdMS_TO_TICKS(2000))
                g_state.can_ok = false;

            twai_status_info_t st;
            if (twai_get_status_info(&st) == ESP_OK &&
                st.state == TWAI_STATE_BUS_OFF) {
                ESP_LOGW(TAG, "Bus-off — recovering");
                twai_stop();
                twai_driver_uninstall();
                vTaskDelay(pdMS_TO_TICKS(100));
                if (twai_install_and_start() == ESP_OK)
                    ESP_LOGI(TAG, "TWAI recovered");
            }
        }
    }
}

// ── 公开 API ─────────────────────────────────────────────────────────────
void can_direct_start(void)
{
    ESP_ERROR_CHECK(twai_install_and_start());
    ESP_LOGI(TAG, "TWAI started: TX=IO%d RX=IO%d 500kbps", CAN_TX_PIN, CAN_RX_PIN);
    xTaskCreatePinnedToCore(can_rx_task, "can_rx", 4096, NULL, 5, NULL, 0);
}
