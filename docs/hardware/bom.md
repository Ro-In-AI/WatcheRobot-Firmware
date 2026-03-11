# Bill of Materials (BOM)

> Hardware required to build and run WatcheRobot

---

## Core Hardware

| Item | Part | Quantity | Notes |
|------|------|----------|-------|
| Main board | SenseCAP Watcher | 1 | ESP32-S3, 16MB Flash, 8MB PSRAM, Himax HX6538, SPD2010 LCD |
| USB-C cable | Any USB-C data cable | 1 | For flashing and power |

## Servo Gimbal

| Item | Specification | Quantity | GPIO |
|------|--------------|----------|------|
| Servo X-axis | Standard PWM servo, 5V, 180° | 1 | GPIO 19 |
| Servo Y-axis | Standard PWM servo, 5V, 180° | 1 | GPIO 20 |
| Servo power supply | 5V / 2A minimum | 1 | External (do not power servos from USB) |

> ⚠️ **Power warning**: Servos draw significant current. Use a dedicated 5V power supply. The ESP32-S3 USB power (500mA) is insufficient for servo loads.

## Optional

| Item | Purpose |
|------|---------|
| 3D-printed gimbal frame | Mechanical mounting for servos |
| Mounting screws | M2 or M3 depending on servo horn |

---

## Software Dependencies

| Dependency | Source | Version |
|------------|--------|---------|
| ESP-IDF | Espressif | v5.2.1 |
| sensecap-watcher BSP | Seeed Studio (included) | — |
| sscma_client | Seeed Studio (included) | — |
| esp_lvgl_port | Espressif (included) | — |
| LVGL | LVGL (included) | v8.x |
| ESP-SR (wake word) | Espressif | via ESP-IDF |

## Cloud Server Dependencies

| Dependency | Install |
|------------|---------|
| Python 3.10+ | `pip install -r requirements.txt` |
| Aliyun ASR API key | Aliyun Console |
| Volcengine TTS API key | Volcengine Console |
| Anthropic API key (Claude) | console.anthropic.com |
