# WatcheRobot S3 通讯协议冻结基线（2026-03-17）

## 1. 目标与范围
- 目标：给当前可运行固件定义一套可联调、可回归的“冻结协议基线”。
- 范围：只覆盖 `main` 启动链路中实际生效的外部通信协议，不把未实现 stub 能力纳入强制冻结。

## 2. 当前生效通信链路（按启动顺序）
1. `Wi-Fi STA` 连接路由器。
2. `UDP Broadcast Discovery` 发现服务端 IP/端口。
3. `WebSocket` 建链并收发控制消息、音频流。

## 3. 现在必须冻结（Freeze Now）

### FZ-01 Wi-Fi 接入层
- 连接模式：`STA`。
- 初版凭据来源：固件内置 SSID/PASS（当前实现是硬编码）。
- 连接超时：`10s`；断线自动重连。
- 鉴权阈值：`WPA2_PSK`。

### FZ-02 UDP Discovery 协议
- 传输：`UDP Broadcast`。
- 端口：`37020`。
- 设备请求报文（设备 -> 服务端）：
  - JSON：`{"cmd":"DISCOVER","device_id":"watcher-XXXX","mac":"XX:XX:XX:XX:XX:XX"}`
- 服务端响应报文（服务端 -> 设备）：
  - JSON：`{"cmd":"ANNOUNCE","ip":"x.x.x.x","port":8765,"version":"1.0.0"}`
- 超时与重试：
  - 总超时：`30s`
  - 每轮重试上限：`3`
  - 间隔：`5s`

### FZ-03 WebSocket 连接层
- URL 生成规则：`ws://<discovered_ip>:<port>`。
- 连接超时：`10s`。
- 当前冻结为 `ws://`（明文）；`wss://` 不在本次冻结范围。

### FZ-04 WebSocket 控制消息（JSON）协议
- 顶层统一字段：`type`（必需），`code`（可选），`data`（按类型变化）。
- 设备需支持的下行消息类型：
  - `servo`
  - `display`
  - `status`
  - `asr_result`
  - `bot_reply`
  - `tts_end`
  - `error`
  - `capture`（当前 no-op，但类型保留）
  - `reboot`
- `servo` 字段冻结：
  - `data.id`: `"x"` / `"y"`（大小写兼容）
  - `data.angle`: `0..180`（兼容 `angle`/`Angle`）
  - `data.time`: ms，默认 `100`
  - 当 `angle` 缺失时默认 `90`
- `display` 字段冻结：
  - `data.text`: 最长按协议层接收 128
  - `data.emoji`: 最长 16
  - `data.size`: 可选，默认 UI 自定
- 文本类字段上限冻结：
  - `status.data`：256
  - `asr_result.data`：256
  - `bot_reply.data`：256
  - `error.data`：256

### FZ-05 WebSocket 音频流协议
- 上行（设备 -> 服务端）：
  - 二进制帧：裸 `PCM`，`16-bit`，`16kHz`，`mono`
  - 当前发送周期：约 `60ms/帧`（典型 `1920 bytes`）
  - 录音结束标记：文本帧 `"over"`
- 下行（服务端 -> 设备）：
  - 二进制帧：裸 `PCM`，`16-bit`，`24kHz`，`mono`
  - 结束标记：JSON `{"type":"tts_end", ...}`

### FZ-06 执行约束（联调必须知晓）
- 舵机：
  - `X` 轴范围 `0..180`
  - `Y` 轴最终被机械保护钳位到配置区间（当前默认 `90..150`）
- 显示文本：
  - 终端显示层最终会截断到 `30` 字符并追加 `...`

## 4. 暂不冻结（Freeze Later）
- BLE GATT + Wi-Fi Provisioning（当前为 stub）。
- Camera 服务与 Camera HAL（当前为 stub）。
- OTA 下载与升级流程（当前为 stub）。
- SSCMA 视觉链路对云侧的报文协议（当前未进入主链路联调）。

## 5. 协议版本统一建议（本轮要拍板）
- 现状存在 `v2.0` 与 `v2.1` 注释混用。
- 建议本轮统一对外标识为：`Watcher-WS-Protocol v2.1.0`。
- 向后兼容要求（冻结）：
  - `servo` 同时接受 `angle` 与 `Angle`
  - 保持 `"over"` + `"tts_end"` 结束语义不变

## 6. 联调验收最小用例（建议）
1. Discovery 成功：设备 30s 内拿到 `ANNOUNCE` 并建立 WS。
2. Servo：`x/y`、`angle/Angle`、默认值路径都可执行。
3. 语音上行：持续发送二进制 PCM，结束后发送 `"over"`。
4. TTS 下行：收到二进制 PCM 播放，收到 `tts_end` 正确收尾。
5. Display/Status/Error：表情映射与文本截断行为一致。

## 7. 风险记录（不阻塞本次冻结）
- Discovery 与 WS 目前均未做链路鉴权/加密。
- Wi-Fi 仍为硬编码凭据。
- 个别注释仍保留“Opus”历史描述，和当前裸 PCM 实现不一致（文档需后续对齐）。
