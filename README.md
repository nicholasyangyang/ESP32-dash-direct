# ESP32-dash-direct

Tesla Model 3/Y（HW4）CAN 总线直连仪表盘。

TWAI 直接接收 CAN 帧，解析后通过 LVGL 实时渲染到 320×240 LCD，**无需 WiFi，无需网络，开机即显示**。

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
┌──────────────────────────────────────┐
│  [左面板 180px]   │  [右面板 138px]  │
│                   │                  │
│      87           │  Range           │
│      km/h         │  342 km          │
│                   │                  │
│  P  R  N  D       │  OutTemp         │
│                   │  18°C            │
│  78%  ▓▓▓▓▓▓░░   │                  │
│                   │  BattTemp        │
│                   │  32°C            │
│                   │                  │
│                   │  ← Regen         │
│                   │  ▓▓▓▓░░░░░░░    │
├──────────────────────────────────────┤
│  LO  HI  ←  →  ⚠  ⚡  AP           │
└──────────────────────────────────────┘
```

| 区域 | 内容 |
|------|------|
| 左面板 | 速度（大字）、档位（P/R/N/D 高亮当前）、SOC 进度条 |
| 右面板 | 续航里程、车外温度、电池最高温度、回收强度进度条 |
| 状态栏 | 近光 / 远光 / 左转 / 右转 / 双闪 / 充电 / AP 图标 |

---

## CAN 信号解析（HW4）

### 车速 — `0x257` DI_speed

| 字节 | 说明 | 解析 |
|------|------|------|
| [1..2] | 速度（小端） | `(d[1] \| d[2]<<8) * 0.1` km/h |

**范围：** 0.0 ~ 250.0 km/h

---

### 档位 — `0x118` DI_gear

| 字节 | Bit | 说明 |
|------|-----|------|
| [1] | 7:5 | `(d[1]>>5) & 0x07` |

| 值 | 档位 |
|----|------|
| 0 | P |
| 1 | R |
| 2 | N |
| 3 | D |

---

### 续航 — `0x306` DI_range

| 字节 | 说明 | 解析 |
|------|------|------|
| [0..1] | 剩余里程（小端） | `(d[0] \| d[1]<<8) * 0.1` km |

**范围：** 0.0 ~ 600.0 km

---

### 电池 SOC — `0x132` BMS_socUI

| 字节 | 说明 | 解析 |
|------|------|------|
| [0..1] | SOC（小端） | `(d[0] \| d[1]<<8) * 0.1` % |

**范围：** 0.0 ~ 100.0 %

---

### 充电状态 — `0x292` BMS_chargeStatus

| 字节 | Bit | 说明 |
|------|-----|------|
| [0] | 0 | `1` = 正在充电 |

---

### 车外温度 — `0x528` VCFRONT_hvac

| 字节 | 说明 | 解析 |
|------|------|------|
| [2] | 外部温度（int8） | `(int8_t)d[2]` °C |

**范围：** -40 ~ +60 °C

---

### 电池最高温度 — `0x312` BMS_packStatus

| 位域 | 说明 | 解析 |
|------|------|------|
| bits 53..61（9-bit，小端） | 电池温度 | `raw = ((d[6]>>5)&0x07) \| ((d[7]&0x3F)<<3)`<br>`temp = raw * 0.25 - 25.0` °C |

**范围：** -25.0 ~ +102.75 °C，精度 0.25 °C

---

### 回收强度 — `0x334` UI_powertrainControl

| 字节 | 说明 | 解析 |
|------|------|------|
| [3] | 回收百分比 | `d[3] * 0.5` % |

**范围：** 0.0 ~ 100.0 %，精度 0.5 %

---

### 灯光状态 — `0x3F5` VCFRONT_vehicleLights

| 字段 | 字节/位 | 条件 |
|------|---------|------|
| 近光 `low_beam` | d[3] bits 5:4 或 7:6 | `== 1` |
| 远光 `high_beam` | d[4] bits 1:0 或 3:2 | `== 1` |
| 双闪 `hazard` | d[0] bits 7:4 | `!= 0` |
| 左转 `turn_left` | d[6] bits 3:2 | `== 1` |
| 右转 `turn_right` | d[6] bits 5:4 | `== 1` |

---

### Autopilot 状态 — `0x399` DAS_status

| 字节 | Bit | 说明 |
|------|-----|------|
| [0] | 3:0 | AP 状态码 |

| 值 | 含义 | `ap_engaged` |
|----|------|--------------|
| 0 | 未激活 | false |
| 3 | ACTIVE_NOMINAL | **true** |
| 4 | RESTRICTED | **true** |
| 5 | NAV_AP | **true** |

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

总线恢复后自动继续接收，`can_ok` 状态指示灯图标实时反映总线健康状态。

---

## 对比其他项目

| 项目 | 功能 | 网络 | CAN |
|------|------|------|-----|
| `ESP32-can` | CAN 网关，WebSocket 推送，Web UI | WiFi AP | IO5/IO4 |
| `Esp32-dashboard` | LVGL 仪表盘，WebSocket 接收 | WiFi STA | 无 |
| **`ESP32-dash-direct`** | **CAN 直连显示，无网络** | **无** | **IO10/IO11** |
