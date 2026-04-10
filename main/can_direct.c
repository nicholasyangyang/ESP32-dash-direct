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

// ── Frame parsing — HW4, joshwardell/model3dbc + tesla-can-explorer ──────
//
// 0x257  DI_speed (599):
//        DI_uiSpeed        : 24|9@1+  (1,0)   UI integer
//        DI_uiSpeedUnits   : 33|1@1+           0=mph 1=kph
//
// 0x118  DI_systemStatus (280):
//        DI_gear           : 21|3@1+  (1,0)  0=SNA 1=P 2=R 3=N 4=D
//
// 0x212  BMS_status (530):
//        BMS_chargeRequest  : 29|1@1+           ← charging flag
//
// 0x3F5  VCFRONT_vehicleLights (1013):
//        VCFRONT_indicatorLeftRequest  :  0|2@1+  (1=on)
//        VCFRONT_indicatorRightRequest :  2|2@1+  (1=on)
//        VCFRONT_hazardLightRequest    :  4|4@1+  (!=0 → hazard)
//        VCFRONT_lowBeamLeftStatus     : 28|2@1+
//        VCFRONT_lowBeamRightStatus    : 30|2@1+
//        VCFRONT_highBeamLeftStatus    : 32|2@1+
//        VCFRONT_highBeamRightStatus   : 34|2@1+
//
// 0x399  DAS_status (921):
//        DAS_autopilotState : 0|4@1+  3/4/5 = engaged

static void parse_frame(uint32_t id, uint8_t dlc, const uint8_t *d)
{
    switch (id) {

    // ── 车速 ─────────────────────────────────────────────────────────────
    case 0x257:
        // DI_uiSpeed      : 24|9@1+ (1,0)      → UI 显示速度整数（mph 或 kph），9-bit
        // DI_uiSpeedUnits : 33|1@1+ 0=mph 1=kph
        // DI_vehicleSpeed : 12|12@1+ (0.08,-40) kph → 精确速度
        if (dlc >= 5) {
            uint16_t raw_ui = (uint16_t)d[3] | ((uint16_t)(d[4] & 0x01) << 8);  // bits 24|9
            uint8_t  units  = (d[4] >> 1) & 0x01;     // bit33: 0=mph 1=kph
            uint16_t raw_v  = ((uint16_t)d[1] >> 4) | ((uint16_t)d[2] << 4);
            float    v_kph  = raw_v * 0.08f - 40.0f;
            if (v_kph < 0.0f) v_kph = 0.0f;
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

    // ── 充电状态 ──────────────────────────────────────────────────────────
    case 0x212:
        // BMS_chargeRequest : 29|1 → bit29 = byte3.bit5
        if (dlc >= 4)
            g_state.charging = (d[3] >> 5) & 0x01;
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
    uint32_t cnt_total = 0, cnt_257 = 0, cnt_118 = 0;

    for (;;) {
        twai_message_t msg;
        if (twai_receive(&msg, pdMS_TO_TICKS(200)) == ESP_OK) {
            cnt_total++;
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
                              "  → speed=%.0f  gear=%u",
                         cnt_total, cnt_257, cnt_118,
                         g_state.speed_kmh, g_state.gear);
                cnt_total = cnt_257 = cnt_118 = 0;
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
