#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp.h"
#include "ui.h"
#include "can_direct.h"

#define TAG "main"

void app_main(void)
{
    // 1. 硬件初始化
    ESP_ERROR_CHECK(bsp_i2c_init());
    pca9557_init();
    bsp_lvgl_start();

    // 2. 显示仪表盘界面（CAN 数据到来前显示初始零值）
    ui_init();
    ui_show_dashboard();

    // 3. 启动 CAN 直接接收 (IO10=TX, IO11=RX, 500kbps, HW4)
    can_direct_start();
    ESP_LOGI(TAG, "Running — CAN -> Display direct mode");

    // 4. LVGL 由 esp_lvgl_port 内部 task 驱动，主任务保活
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
