# WatcheRobot S3 通讯协议冻结基线（2026-03-23）

## 1. 目标与范围
- 目标：给当前可运行固件定义一套可联调、可回归的“冻结协议基线”。
- 范围：只覆盖 `main` 启动链路中实际生效的外部通信协议，不把未实现 stub 能力纳入强制冻结。

## 2. 当前生效通信链路（按启动顺序）
1. `Wi-Fi STA` 连接路由器。
2. `UDP Broadcast Discovery` 发现服务端 IP/端口。
3. `WebSocket` 建链并收发控制消息、音频流、视频控制与视频帧。

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
  - `ctrl.servo.angle`
  - `ctrl.robot.state.set`
  - `ctrl.camera.video_config`
  - `ctrl.camera.capture_image`
  - `ctrl.camera.start_video`
  - `ctrl.camera.stop_video`
  - `evt.asr.result`
  - `evt.ai.status`
  - `evt.ai.thinking`
  - `evt.ai.reply`
  - `sys.ping`
- 设备当前支持的上行控制/状态类型：
  - `sys.ack`
  - `sys.nack`
  - `sys.pong`
  - `evt.device.firmware`
  - `evt.device.error`
  - `evt.camera.state`
  - `evt.servo.position`
- `ctrl.servo.angle` 字段冻结：
  - `data.x_deg`: `0..180`
  - `data.y_deg`: `0..180`
  - `data.duration_ms`: 必填，`>0`
- `ctrl.robot.state.set` 字段冻结：
  - `data.command_id`: 可选
  - `data.state_id`: 必填
  - 当前板端内置状态集合：`boot / standby / listening / thinking / processing / speaking / happy / error / custom1 / custom2 / custom3`
  - 状态只下发 `state_id`，完整动作序列、表情动画与音效时间轴由嵌入式端从 `/spiffs/behavior/states.json` 本地执行
- `evt.ai.status` 兼容映射冻结：
  - `idle -> standby`
  - `thinking -> thinking`
  - `processing/analyzing -> processing`
  - `speaking -> speaking`
  - `done/completed -> happy`
  - `error/fail -> error`
- 文本类字段上限冻结：
  - `evt.asr.result.data.text`：256
  - `evt.ai.status.data.message`：256
  - `evt.ai.thinking.data.content`：256
  - `evt.ai.reply.data.text`：256
- `ctrl.camera.video_config` 字段冻结：
  - `data.command_id`: 必填
  - `data.width`: 可选，期望宽度
  - `data.height`: 可选，期望高度
  - `data.fps`: 可选，期望帧率
  - `data.quality`: 可选，建议质量
- `ctrl.camera.capture_image` / `ctrl.camera.start_video` / `ctrl.camera.stop_video`：
  - `data.command_id`: 必填
- `sys.ack` / `sys.nack`：
  - `data.command_id`: 回指控制命令
  - `data.type`: 原始命令类型
  - `data.reason`: 仅 `sys.nack` 使用

### FZ-05 WebSocket 音频流协议
- 上行（设备 -> 服务端）：
  - 二进制帧：`WSPK` 帧头 + 裸 `PCM`
  - `frame_type = 1`
  - `payload`: `16-bit`，`16kHz`，`mono`
  - 当前发送周期：约 `60ms/帧`（典型 `1920 bytes`）
  - 录音结束标记：发送 `frame_type = 1` 且 `LAST` 置位的零负载包
- 下行（服务端 -> 设备）：
  - 二进制帧：`WSPK` 帧头 + 裸 `PCM`
  - `frame_type = 1`
  - `payload`: `16-bit`，`24kHz`，`mono`
  - TTS 结束标记：发送 `LAST` 置位的最后一个音频包，不再使用 `"over"` / `tts_end`
  - 本地音效与云端 TTS 互斥；若 TTS 正在播放，状态脚本中的本地音效轨会被跳过

### FZ-06 WebSocket 视频协议
- camera media 详细定义以 [SERVER_GATEWAY_CAMERA_MEDIA_PROTOCOL.md](D:\GithubRep\WatcheRobot-Firmware\firmware\s3\docs\SERVER_GATEWAY_CAMERA_MEDIA_PROTOCOL.md) 为准。
- 本轮冻结的核心结论：
  - 单帧图片 = `JPEG`
  - 连续视频 = `MJPEG` 风格的 `JPEG frame sequence`
  - camera media 二进制帧统一使用 `WSPK` 16B 帧头
- 单帧抓拍控制：
  - `{"type":"ctrl.camera.capture_image","code":0,"data":{"command_id":"cam-shot-001"}}`
- 视频流开始控制：
  - `{"type":"ctrl.camera.start_video","code":0,"data":{"command_id":"cam-start-001","fps":5}}`
- 视频流停止控制：
  - `{"type":"ctrl.camera.stop_video","code":0,"data":{"command_id":"cam-stop-001"}}`
- 受理成功响应：
  - `{"type":"sys.ack","code":0,"data":{"command_id":"cam-start-001","command_type":"ctrl.camera.start_video","stream_id":12}}`
- camera 状态事件：
  - `{"type":"evt.camera.state","code":0,"data":{"action":"start_video","state":"started","stream_id":12,"fps":5,"message":"format=mjpeg"}}`
- 图片二进制帧：
  - `frame_type = 3`
  - `payload = 完整 JPEG`
- 视频二进制帧：
  - `frame_type = 2`
  - 每个二进制包负载都是一帧完整 `JPEG`
- 当前冻结约束：
  - 单帧和连续流都复用同一相机回调链路
  - 服务端必须按 `WSPK` 帧头解析 camera media
  - 服务端不能把它当 H.264/H.265 码流处理
  - 当前版本不承诺音频上行与视频上行的并发多路复用顺序语义；联调时默认不要同时开启双上行流

### FZ-07 执行约束（联调必须知晓）
- 舵机：
  - `X` 轴范围 `0..180`
  - `Y` 轴最终被机械保护钳位到配置区间（当前默认 `90..150`）
- 显示文本：
  - 终端显示层最终会截断到 `30` 字符并追加 `...`

## 4. 暂不冻结（Freeze Later）
- BLE GATT + Wi-Fi Provisioning（当前为 stub）。
- OTA 下载与升级流程（当前为 stub）。
- 更复杂的视频多路复用、分片与重传策略。
- SSCMA 推理结果对云侧的结构化上报协议。

## 5. 协议版本统一建议（本轮要拍板）
- 现状存在 `v2.0` 与 `v2.1` 注释混用。
- 建议本轮统一对外标识为：`Watcher-WS-Protocol v2.1.0`。
- 向后兼容要求（冻结）：
  - BLE 继续兼容旧文本舵机命令：`X:` / `Y:` / `SET_SERVO:` / `SERVO_MOVE:` / `PING`
  - `evt.ai.status` 继续接受历史状态词：`analyzing` / `completed` / `fail`

## 6. 联调验收最小用例（建议）
1. Discovery 成功：设备 30s 内拿到 `ANNOUNCE` 并建立 WS。
2. Servo：`ctrl.servo.angle` 的 `x_deg/y_deg/duration_ms` 路径可执行。
3. 状态控制：收到 `ctrl.robot.state.set` 后，板端按本地 `states.json` 同步执行动作序列与表情动画。
4. 语音上行：持续发送 `WSPK + PCM`，结束后发送 `LAST` 音频包。
5. TTS 下行：收到 `WSPK + PCM` 播放，收到 `LAST` 音频包后正确收尾。
6. Capture single：收到 `ctrl.camera.capture_image` 后，设备返回 `sys.ack`，并上行 1 个 `frame_type=3` 的 `WSPK + JPEG` 图片帧。
7. Capture stream：收到 `ctrl.camera.start_video` 后，设备返回 `sys.ack` 与 `evt.camera.state(started)`，随后持续上行 `frame_type=2` 的 `WSPK + JPEG` 视频帧；收到 `ctrl.camera.stop_video` 后发送结束包并上报 `evt.camera.state(stopped)`。
8. BLE 兼容：旧文本舵机命令与新的 JSON `ctrl.robot.state.set` 都能正确执行。

## 7. 风险记录（不阻塞本次冻结）
- Discovery 与 WS 目前均未做链路鉴权/加密。
- Wi-Fi 仍为硬编码凭据。
- 个别注释仍保留“Opus”历史描述，和当前裸 PCM 实现不一致（文档需后续对齐）。
- 当前 camera media 走 `JPEG/MJPEG`，不是压缩视频码流；带宽与 CPU 成本都由服务端/网关承担解 JPEG。
