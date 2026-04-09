# ESP32-dash-direct

Tesla Model 3/Y（HW4）CAN 总线直连仪表盘。

TWAI 直接接收 CAN 帧，解析后通过 LVGL 实时渲染到 320×240 LCD，**无需 WiFi，无需网络，开机即显示**。

信号解析基于 [joshwardell/model3dbc](https://github.com/joshwardell/model3dbc) DBC 文件及 [tesla-can-explorer.gapinski.eu](https://tesla-can-explorer.gapinski.eu)（固件 2026.2）交叉验证。

---

## 架构

```
Tesla CAN 总线
      │  500kbps
      ▼
CAN 收发器（SN65HVD230 / TJA1051 等）
      │  IO10(RX) / IO11(TX)
      ▼
┌─────────────────────────────────────────┐
│            ESP32-S3                     │
│                                         │
│  can_rx_task (Core 0, pri 5)            │
│  ┌─────────────────────────────────┐    │
│  │  TWAI driver                   │    │
│  │  parse_frame()                  │    │
│  │  → g_state (dashboard_state_t) │    │
│  └─────────────────────────────────┘    │
│                                         │
│  esp_lvgl_port task (Core 1)            │
│  ┌─────────────────────────────────┐    │
│  │  ui_refresh() @ 60Hz            │    │
│  │  读取 g_state → 更新 LVGL 控件  │    │
│  │  → SPI DMA → ST7789 LCD        │    │
│  └─────────────────────────────────┘    │
└─────────────────────────────────────────┘
```

### 数据流

```
CAN 帧 → twai_receive() → parse_frame() → g_state → lv_timer → 屏幕
```

无锁设计：`g_state` 字段均为 `float`/`bool`（32-bit 对齐），单写多读，ESP32-S3 无需显式互斥锁。

---

## 硬件引脚

### CAN（TWAI）

| 信号 | GPIO | 说明 |
|------|------|------|
| CAN RX | IO10 | 接收发器 RXD |
| CAN TX | IO11 | 接收发器 TXD |

> 波特率：500 kbps，标准帧，接受全部 ID。

### LCD（ST7789，SPI3）

| 信号 | GPIO |
|------|------|
| MOSI | IO40 |
| SCLK | IO41 |
| DC   | IO39 |
| BL   | IO42 |
| CS   | NC（PCA9557 扩展芯片控制） |

### I2C（触摸 + IO 扩展）

| 信号 | GPIO |
|------|------|
| SDA  | IO1  |
| SCL  | IO2  |

> I2C 设备：PCA9557（IO 扩展，LCD CS/背光控制）、FT5x06（触摸，可选）

---

## 显示界面

```
┌────────────────────────────────────────────────┐
│  [左面板 180px]   │ [右面板 76px] │ [TPMS 60px]│
│                   │               │             │
│       87          │  Range        │  TPMS       │
│      km/h         │  342 km       │  bar        │
│                   │               │             │
│  P  R  N  D       │  OutTemp      │  Front      │
│                   │  18°C         │FL    FR      │
│  78%  ▓▓▓▓▓▓░░   │               │2.9   2.9    │
│                   │  BattTemp     │             │
│                   │  32°C         │  Rear       │
│                   │               │RL    RR      │
│                   │  ← Regen      │2.8   2.8    │
│                   │  45kW         │             │
│                   │  ▓▓▓▓░░░░░   │             │
├────────────────────────────────────────────────┤
│  LO  HI  ←  →  ⚠  ⚡  AP                      │
└────────────────────────────────────────────────┘
```

| 区域 | 内容 |
|------|------|
| 左面板 | 速度（大字）、档位（P/R/N/D 高亮当前）、SOC% + 进度条 |
| 右面板 | 续航里程、车外温度、电池最高温度、回收功率（kW）+ 进度条 |
| TPMS 列 | 四轮胎压（bar），颜色指示正常/偏低/过高 |
| 状态栏 | 近光 / 远光 / 左转 / 右转 / 双闪 / 充电 / AP 图标 |

---

## CAN 信号解析（HW4）

> 参考：`joshwardell/model3dbc` DBC + `tesla-can-explorer.gapinski.eu` fw 2026.2

### 车速 — `0x257` DI_speed (dec 599)

| 信号 | 位域 | 解析 | 说明 |
|------|------|------|------|
| `DI_uiSpeed` | `24\|9@1+` | `d[3] \| ((d[4] & 0x01) << 8)` | UI 显示整数（mph 或 kph），9-bit |
| `DI_uiSpeedUnits` | `33\|1@1+` | `(d[4] >> 1) & 0x01` | `0`=mph `1`=kph |
| `DI_vehicleSpeed` | `12\|12@1+` | `raw * 0.08 - 40.0` kph | 精确速度（未使用） |

如仪表单位为 mph，则 `speed_kmh = uiSpeed × 1.60934`，否则直接取整数值。

---

### 档位 — `0x118` DI_systemStatus (dec 280)

| 信号 | 位域 | 解析 |
|------|------|------|
| `DI_gear` | `21\|3@1+` | `(d[2] >> 5) & 0x07` |

| DBC 值 | 档位 | `g_state.gear` |
|--------|------|----------------|
| 0 | SNA | 0xFF（无效） |
| 1 | P | 0 |
| 2 | R | 1 |
| 3 | N | 2 |
| 4 | D | 3 |

---

### 续航里程 — `0x33A` UI_range (dec 826)

主信号源，同时提供 SOC 整数百分比。

| 信号 | 位域 | 解析 | 说明 |
|------|------|------|------|
| `UI_Range` | `0\|10@1+` | `d[0] \| ((d[1] & 0x03) << 8)` | 仪表显示里程（km 或 mi，与车辆单位一致） |
| `UI_idealRange` | `16\|10@1+` | — | 理想里程（未使用） |
| `UI_ratedWHpM` | `32\|10@1+` | — | 能耗 Wh/mi（未使用） |
| `UI_SOC` | `48\|7@1+` | `d[6] & 0x7F` | 显示电量整数 0–100 % |
| `UI_uSOE` | `56\|7@1+` | — | 未使用 |

---

### 电量百分比 — `0x292` BMS_socStatus (dec 658)

备用 SOC 源（仅在 0x33A 未收到数据时使用，精度更高 0.1%）。

| 信号 | 位域 | 解析 | 说明 |
|------|------|------|------|
| `SOCmin292` | `0\|10@1+` | × 0.1 % | 最低单体 SOC |
| `SOCUI292` | `10\|10@1+` | `((d[1]>>2)&0x3F) \| ((d[2]&0x0F)<<6)`，× 0.1 % | 显示 SOC ← **使用** |
| `SOCmax292` | `20\|10@1+` | × 0.1 % | — |
| `SOCave292` | `30\|10@1+` | × 0.1 % | — |
| `BMS_battTempPct` | `50\|8@1+` | × 0.4 % | — |

---

### 能量回收（最大回收功率）— `0x252` BMS_powerAvailable (dec 594)

> **注意**：旧版社区 DBC 误将此 ID 标注为 `DI_state`（DI_uiRange）。  
> joshwardell/model3dbc 及 tesla-can-explorer 均确认为 `BMS_powerAvailable`。

| 信号 | 位域 | 解析 | 说明 |
|------|------|------|------|
| `BMS_maxRegenPower` | `0\|16@1+` | `(d[0] \| d[1]<<8) * 0.01` kW | 电池当前可接受的最大回收功率 ← **使用** |
| `BMS_maxDischargePower` | `16\|16@1+` | × 0.013 kW | 最大放电功率 |
| `BMS_maxStationaryHeatPower` | `32\|10@1+` | × 0.01 kW | — |
| `BMS_notEnoughPowerForHeatPump` | `42\|1@1+` | — | — |
| `BMS_powerLimitsState` | `48\|1@1+` | `0`=未计算 `1`=已为驾驶计算 | — |

`regen_kw` 反映电池的实时回收能力（受温度、SOC、充电状态影响）。UI 进度条满格 = 200 kW。

---

### 回收强度设定 — `0x334` UI_powertrainControl (dec 820)

用户在屏幕上设定的最大回收扭矩上限（Standard / Low 模式对应不同值）。

| 信号 | 位域 | 解析 | 说明 |
|------|------|------|------|
| `UI_regenTorqueMax` | `24\|8@1+` | `d[3] * 0.5` % | 回收扭矩限制 0–100 % |
| `UI_stoppingMode` | `40\|2@1+` | — | `0`=Standard `1`=Creep `2`=Hold |

---

### 充电状态 — `0x212` BMS_status (dec 530)

| 信号 | 位域 | 解析 |
|------|------|------|
| `BMS_chargeRequest` | `29\|1@1+` | `(d[3] >> 5) & 0x01`，`1`=充电中 |

---

### 车外温度 — `0x321` VCFRONT_sensors (dec 801)

> `0x528` 帧为 `UnixTime`（Unix 时间戳），**不含温度**。

| 信号 | 位域 | 解析 | 说明 |
|------|------|------|------|
| `VCFRONT_tempAmbient` | `24\|8@1+` | `d[3] * 0.5 - 40.0` °C | 环境温度 |
| `VCFRONT_tempAmbientFiltered` | `40\|8@1+` | `d[5] * 0.5 - 40.0` °C | 滤波后（未使用） |

范围：−40 ~ +80 °C，精度 0.5 °C。

---

### 电池最高温度 — `0x312` BMS_packStatus (dec 786)

| 信号 | 位域 | 解析 |
|------|------|------|
| `BMSmaxPackTemperature` | `53\|9@1+` | `raw = ((d[6]>>5)&0x07) \| ((d[7]&0x3F)<<3)`<br>`temp = raw * 0.25 - 25.0` °C |

精度 0.25 °C，范围 −25.0 ~ +102.75 °C。

---

### 灯光状态 — `0x3F5` VCFRONT_vehicleLights (dec 1013)

| 信号 | 位域 | 条件 | 字段 |
|------|------|------|------|
| `VCFRONT_indicatorLeftRequest` | `0\|2@1+` | `== 1` | `turn_left` |
| `VCFRONT_indicatorRightRequest` | `2\|2@1+` | `== 1` | `turn_right` |
| `VCFRONT_hazardLightRequest` | `4\|4@1+` | `!= 0` | `hazard` |
| `VCFRONT_lowBeamLeftStatus` | `28\|2@1+` | `== 1` | `low_beam` |
| `VCFRONT_lowBeamRightStatus` | `30\|2@1+` | `== 1` | `low_beam` |
| `VCFRONT_highBeamLeftStatus` | `32\|2@1+` | `== 1` | `high_beam` |
| `VCFRONT_highBeamRightStatus` | `34\|2@1+` | `== 1` | `high_beam` |

---

### Autopilot 状态 — `0x399` DAS_status (dec 921)

| 信号 | 位域 | 解析 |
|------|------|------|
| `DAS_autopilotState` | `0\|4@1+` | `d[0] & 0x0F` |

| 值 | 含义 | `ap_engaged` |
|----|------|--------------|
| 3 | ACTIVE_NOMINAL | true |
| 4 | RESTRICTED | true |
| 5 | NAV_AP | true |
| 其他 | 未激活 | false |

---

### 胎压 — `0x219` VCSEC_TPMSData

| 字节 | 说明 | 解析 |
|------|------|------|
| `d[0] & 0x03` | 轮位：`0`=FL `1`=FR `2`=RL `3`=RR | — |
| `d[1]` | 胎压原始值 | `d[1] * 0.025` bar |

颜色阈值：< 2.0 bar 或 > 3.5 bar → 红；< 2.5 或 > 3.1 → 橙；正常 → 绿。

---

## g_state 字段一览

| 字段 | 类型 | 来源 | 说明 |
|------|------|------|------|
| `speed_kmh` | float | 0x257 | 车速 km/h |
| `gear` | uint8 | 0x118 | 0=P 1=R 2=N 3=D 0xFF=无效 |
| `range_km` | float | 0x33A | 续航里程（km 或 mi，与车辆单位一致） |
| `soc_pct` | float | 0x33A / 0x292 | 电量百分比 % |
| `charging` | bool | 0x212 | 是否充电中 |
| `temp_outside_c` | float | 0x321 | 车外温度 °C（VCFRONT_tempAmbient） |
| `batt_max_temp_c` | float | 0x312 | 电池最高温度 °C |
| `regen_kw` | float | 0x252 | **最大回收功率 kW**（BMS_maxRegenPower） |
| `regen_pct` | float | 0x334 | 回收扭矩限制设定 % |
| `low_beam` | bool | 0x3F5 | 近光灯 |
| `high_beam` | bool | 0x3F5 | 远光灯 |
| `turn_left` | bool | 0x3F5 | 左转向 |
| `turn_right` | bool | 0x3F5 | 右转向 |
| `hazard` | bool | 0x3F5 | 双闪 |
| `ap_engaged` | bool | 0x399 | Autopilot 激活 |
| `tpms_fl/fr/rl/rr` | float | 0x219 | 四轮胎压 bar |
| `can_ok` | bool | — | TWAI 总线正常接收中 |

---

## 文件结构

```
ESP32-dash-direct/
├── CMakeLists.txt
├── sdkconfig.defaults          # ESP32-S3, PSRAM, LVGL 字体配置
├── partitions.csv              # nvs + factory 分区
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml       # 依赖：lvgl, esp_lvgl_port, esp_lcd_touch_ft5x06
    ├── main.c                  # 入口：BSP → UI → CAN 启动
    ├── can_direct.h/c          # TWAI 初始化 + HW4 帧解析 + bus-off 恢复
    ├── ui.h/c                  # LVGL 仪表盘界面（60Hz 刷新）
    └── bsp.h/c                 # ESP32-S3 LCD/触摸/I2C/背光驱动
```

---

## 依赖

| 组件 | 版本 | 用途 |
|------|------|------|
| `lvgl/lvgl` | ~8.3.0 | GUI 渲染 |
| `espressif/esp_lvgl_port` | ~1.4.0 | LVGL 与 IDF 集成 |
| `espressif/esp_lcd_touch_ft5x06` | ~1.0.6 | 触摸驱动（可选） |
| ESP-IDF | ≥5.2.0 | 基础框架 |

---

## 编译与烧录

```bash
source ~/esp-idf/export.sh
idf.py build
# 全擦后烧录（首次或更换固件）
idf.py erase-flash flash -p /dev/ttyUSBx
# 仅烧录
idf.py flash -p /dev/ttyUSBx
```

---

## Bus-off 恢复

CAN 总线 bus-off 时（如总线断开再接入），`can_rx_task` 自动执行：

```
twai_stop() → twai_driver_uninstall() → 延时 100ms → 重新安装并启动
```

总线恢复后自动继续接收，`can_ok` 状态实时反映总线健康。

---

## 对比其他项目

| 项目 | 功能 | 网络 | CAN |
|------|------|------|-----|
| `ESP32-can` | CAN 网关，WebSocket 推送，Web UI | WiFi AP | IO5/IO4 |
| `Esp32-dashboard` | LVGL 仪表盘，WebSocket 接收 | WiFi STA | 无 |
| **`ESP32-dash-direct`** | **CAN 直连显示，无网络** | **无** | **IO10/IO11** |
