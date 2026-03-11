# Getting Started

Get WatcheRobot running from scratch in under 30 minutes.

---

## Prerequisites

### Hardware

- SenseCAP Watcher device
- USB-C cable (for flashing)
- Two standard PWM servos (connected to GPIO 19 and GPIO 20)
- PC on the same local network as the device

### Software

| Tool | Version | Install |
|------|---------|---------|
| ESP-IDF | v5.2.1 | [Windows](https://docs.espressif.com/projects/esp-idf/en/v5.2.1/get-started/windows-setup.html) / [Linux/macOS](https://docs.espressif.com/projects/esp-idf/en/v5.2.1/get-started/linux-macos-setup.html) |
| Python | 3.10+ | [python.org](https://www.python.org/downloads/) |
| Git | any | [git-scm.com](https://git-scm.com/) |

---

## Step 1 — Clone the Repository

```bash
git clone https://github.com/Ro-In-AI/WatcheRobot-Firmware.git
cd WatcheRobot-Firmware
```

---

## Step 2 — Configure

Activate the ESP-IDF environment first:

```powershell
# Windows PowerShell
C:\Espressif\frameworks\esp-idf-v5.2.1\export.ps1
```

```bash
# Linux / macOS
. ~/esp/esp-idf/export.sh
```

Open the configuration menu:

```bash
cd firmware/s3
idf.py menuconfig
```

Navigate to **Watcher Configuration** and set:

| Option | Description | Example |
|--------|-------------|---------|
| `WATCHER_WIFI_SSID` | Your WiFi network name | `MyHomeWifi` |
| `WATCHER_WIFI_PASSWORD` | WiFi password | `mypassword` |
| `WATCHER_SERVO_X_GPIO` | X-axis servo GPIO | `19` (default) |
| `WATCHER_SERVO_Y_GPIO` | Y-axis servo GPIO | `20` (default) |
| `WATCHER_BLE_ENABLE` | Enable BLE | `n` to start |

Save and exit (`S` → `Q`).

---

## Step 3 — Build

```bash
idf.py set-target esp32s3
idf.py build
```

A successful build ends with:
```
Project build complete. To flash, run:
  idf.py -p (PORT) flash
```

---

## Step 4 — Flash

Connect the SenseCAP Watcher via USB-C, then:

```bash
# Windows (check Device Manager for the COM port)
idf.py -p COM3 flash monitor

# Linux / macOS
idf.py -p /dev/ttyUSB0 flash monitor
```

The device will boot and show the progress animation. Watch the monitor output for the server discovery step.

---

## Step 5 — Start the Cloud Server

In a separate terminal on your PC:

```bash
# Clone and set up the server (first time only)
git clone https://github.com/Ro-In-AI/watcher-server.git
cd watcher-server
pip install -r requirements.txt

# Copy and fill in API keys
cp .env.example .env
# Edit .env: ALIYUN_ASR_KEY, VOLCENGINE_TTS_KEY, ANTHROPIC_API_KEY

# Run the server
python src/main.py
```

The server listens on port 8765 (WebSocket) and 8767 (UDP discovery).

---

## Step 6 — Test

1. The device should discover the server automatically and show **"Ready"** on the display
2. Press the physical button (long press) or say **"Hi 乐鑫"** to start a conversation
3. Speak — the device records, sends audio to the server, gets a response, and plays TTS

---

## Troubleshooting

### Device shows "Server Not Found"

- Ensure PC and device are on the **same WiFi network**
- Check the server is running (`python src/main.py` shows no errors)
- Check firewall: UDP port 8767 and TCP port 8765 must be accessible

### No audio from speaker

- Check `hal_audio` init log in monitor output
- Verify I2S pin configuration in `sdkconfig`

### Servos not moving

- Check GPIO 19/20 connections (signal wire, power, ground)
- Send a test command via WebSocket: `{"type":"servo","id":"X","angle":90,"time":500}`

### Build fails with "component not found"

```bash
# Refresh managed components
idf.py --no-ccache fullclean
idf.py build
```

---

## Next Steps

- [Architecture overview](architecture.md) — understand the component design
- [GPIO mapping](hardware/gpio-mapping.md) — full pin reference
- [CONTRIBUTING.md](../CONTRIBUTING.md) — how to add features or fix bugs
