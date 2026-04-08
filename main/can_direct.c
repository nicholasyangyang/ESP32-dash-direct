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

// ── Frame parsing — HW4 signals only ────────────────────────────────────
static void parse_frame(uint32_t id, uint8_t dlc, const uint8_t *d)
{
    switch (id) {

    case 0x257:  // DI_speed
        if (dlc >= 3) {
            uint16_t raw = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
            g_state.speed_kmh = raw * 0.1f;
        }
        break;

    case 0x118:  // DI_gear  bits[7:5] of byte[1]
        if (dlc >= 2)
            g_state.gear = (d[1] >> 5) & 0x07;
        break;

    case 0x306:  // DI_range — byte[0..1] = range * 0.1 km
        if (dlc >= 2) {
            uint16_t raw = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
            g_state.range_km = raw * 0.1f;
        }
        break;

    case 0x132:  // BMS_socUI — byte[0..1] = SOC * 0.1 %
        if (dlc >= 2) {
            uint16_t raw = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
            g_state.soc_pct = raw * 0.1f;
        }
        break;

    case 0x292:  // BMS_chargeStatus
        if (dlc >= 1)
            g_state.charging = (d[0] & 0x01) != 0;
        break;

    case 0x528:  // VCFRONT_hvac — byte[2] = outside temp (int8, °C)
        if (dlc >= 3)
            g_state.temp_outside_c = (float)(int8_t)d[2];
        break;

    case 0x312:  // BMS_packStatus — batt max temp, 9-bit little-endian bits[53..61]
        if (dlc >= 8) {
            uint16_t raw = (uint16_t)((d[6] >> 5) & 0x07)
                         | ((uint16_t)(d[7] & 0x3F) << 3);
            g_state.batt_max_temp_c = raw * 0.25f - 25.0f;
        }
        break;

    case 0x334:  // UI_powertrainControl — regen_pct = byte[3] * 0.5
        if (dlc >= 4)
            g_state.regen_pct = d[3] * 0.5f;
        break;

    case 0x3F5:  // VCFRONT_vehicleLights
        if (dlc >= 7) {
            g_state.low_beam   = ((d[3] >> 4) & 0x03) == 1 || ((d[3] >> 6) & 0x03) == 1;
            g_state.high_beam  = ((d[4] >> 0) & 0x03) == 1 || ((d[4] >> 2) & 0x03) == 1;
            g_state.hazard     = ((d[0] >> 4) & 0x0F) != 0;
            g_state.turn_left  = ((d[6] >> 2) & 0x03) == 1;
            g_state.turn_right = ((d[6] >> 4) & 0x03) == 1;
        }
        break;

    case 0x399:  // DAS_status — AP state bits[3:0] of byte[0]
        if (dlc >= 1) {
            uint8_t ap = d[0] & 0x0F;
            g_state.ap_engaged = (ap == 3 || ap == 4 || ap == 5);
        }
        break;

    default:
        break;
    }
}

// ── TWAI install helper (also used for bus-off recovery) ─────────────────
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

// ── RX task ──────────────────────────────────────────────────────────────
static void can_rx_task(void *arg)
{
    (void)arg;
    uint32_t last_rx_tick = 0;

    for (;;) {
        twai_message_t msg;
        if (twai_receive(&msg, pdMS_TO_TICKS(200)) == ESP_OK) {
            parse_frame(msg.identifier, msg.data_length_code, msg.data);
            last_rx_tick   = xTaskGetTickCount();
            g_state.can_ok = true;
        } else {
            // Mark bus unhealthy if no frame for 2 s
            if ((xTaskGetTickCount() - last_rx_tick) > pdMS_TO_TICKS(2000))
                g_state.can_ok = false;

            // Recover from bus-off
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

// ── Public API ───────────────────────────────────────────────────────────
void can_direct_start(void)
{
    ESP_ERROR_CHECK(twai_install_and_start());
    ESP_LOGI(TAG, "TWAI started: TX=IO%d RX=IO%d 500kbps", CAN_TX_PIN, CAN_RX_PIN);
    xTaskCreatePinnedToCore(can_rx_task, "can_rx", 4096, NULL, 5, NULL, 0);
}
