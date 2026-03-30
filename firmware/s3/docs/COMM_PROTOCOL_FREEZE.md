# WatcheRobot S3 通讯协议冻结基线（0.1.0 Beta / 2026-03-30）

## Beta 总结
- 当前 `0.1.0-beta` 版本采用 `Watcher-WS-Protocol v0.1.5` 作为板端对外网络协议基线。
- 当前主线已经完成 Discovery、WebSocket 建链、BLE provisioning、语音上下行、图片/视频媒体帧、行为状态资源映射等主要联调链路的集成。
- 本文档继续作为板端对外通信真基线；后续正式 `0.1.0` 仅在联调收口后再冻结更严格的稳定约束，不改变本轮已明确的主消息方向。

## 1. 目标与范围
- 目标：给当前可运行固件定义一套可联调、可回归的“冻结协议基线”。
- 范围：只覆盖 `main` 启动链路中实际生效的外部通信协议，不把未实现 stub 能力纳入强制冻结。
- 真源：`D:\GithubRep\watcher-server\docs\device_communication_protocol.md`。

## 2. 当前生效通信链路（按启动顺序）
1. `Wi-Fi STA` 连接路由器。
2. `UDP Broadcast Discovery` 发现服务端 `ip / port / version / protocol_version / server`。
3. `WebSocket` 建链。
4. 设备先发送 `sys.client.hello`。
5. 收到服务端 `sys.ack(type=sys.client.hello)` 后进入 `session_ready`。
6. 进入业务阶段后收发 JSON 文本帧，以及 `WSPK` 二进制音频/视频/图片帧。

## 3. 现在必须冻结（Freeze Now）

### FZ-01 Wi-Fi 接入层
- 连接模式：`STA`。
- 当前凭据来源：已保存 STA 配置；若未配置则通过 BLE provisioning 下发。
- 连接超时：`10s`；断线自动重连。
- 鉴权阈值：`WPA2_PSK`。

### FZ-02 UDP Discovery 协议
- 传输：`UDP Broadcast`。
- 端口：`37020`。
- 设备请求报文（设备 -> 服务端）：
  - JSON：`{"cmd":"DISCOVER","device_id":"watcher-XXXX","mac":"XX:XX:XX:XX:XX:XX"}`
- 服务端响应报文（服务端 -> 设备）：
  - JSON：`{"cmd":"ANNOUNCE","ip":"x.x.x.x","port":8765,"version":"1.0.0","protocol_version":"0.1.5","server":"watcher-server"}`
- 超时与重试：
  - 总超时：`30s`
  - 每轮重试上限：`3`
  - 间隔：`5s`
- 协议检查：
  - 固件发现阶段必须校验 `protocol_version`
  - 当前冻结协议版本：`0.1.5`
  - 版本不匹配时不进入 WebSocket 业务阶段

### FZ-03 WebSocket 连接层
- URL 生成规则：`ws://<discovered_ip>:<port>`。
- 连接超时：`10s`。
- 当前冻结为 `ws://`；`wss://` 不在本次冻结范围。
- 建链后首个业务消息必须是：
  - `{"type":"sys.client.hello","code":0,"data":{"role":"hardware","fw_version":"<fw>"}}`
- 业务放行条件：
  - 只有收到 `sys.ack.data.type=sys.client.hello` 后，固件才允许上报业务消息。

### FZ-04 WebSocket 控制消息（JSON）协议
- 顶层统一字段：`type`（必需），`code`（可选），`data`（按类型变化）。
- 当前主协议下，设备需支持的下行消息类型：
  - `ctrl.servo.angle`
  - `ctrl.camera.video_config`
  - `ctrl.camera.capture_image`
  - `ctrl.camera.start_video`
  - `ctrl.camera.stop_video`
  - `evt.asr.result`
  - `evt.ai.status`
  - `evt.ai.thinking`
  - `evt.ai.reply`
  - `xfer.ota.handshake`
  - `xfer.ota.checksum`
  - `sys.ping`
- 当前主协议下，设备上行消息类型：
  - `sys.client.hello`
  - `sys.ack`
  - `sys.nack`
  - `sys.pong`
  - `evt.device.firmware`
  - `evt.device.error`
  - `evt.servo.position`
  - `evt.ota.progress`
  - `xfer.ota.handshake`
- 当前固件保留的兼容扩展：
  - `evt.camera.state`
  - `ctrl.robot.state.set`
- `ctrl.servo.angle` 字段冻结：
  - `data.x_deg`: `0..180`
  - `data.y_deg`: `0..180`
  - `data.duration_ms`: 可选；缺省时固件按 `100ms` 处理
- `evt.ai.status` 兼容映射冻结：
  - `idle -> standby`
  - `thinking -> thinking`
  - `processing/analyzing -> processing`
  - `speaking -> speaking`
  - `done/completed -> happy`
  - `error/fail -> error`
- `evt.ai.status` 资源字段冻结：
  - `data.image_name`: 可选
  - `data.action_file`: 可选
  - `data.sound_file`: 可选
  - 固件收到后优先按 `action_file -> status -> fallback` 顺序解析状态
- 文本类字段上限冻结：
  - `evt.asr.result.data.text`：256
  - `evt.ai.status.data.message`：256
  - `evt.ai.thinking.data.content`：256
  - `evt.ai.reply.data.text`：256
- `ctrl.camera.video_config` 字段冻结：
  - `data.command_id`: 建议携带；若携带则原样回指
  - `data.width`: 可选
  - `data.height`: 可选
  - `data.fps`: 可选
  - `data.quality`: 可选
- `ctrl.camera.capture_image` / `ctrl.camera.start_video` / `ctrl.camera.stop_video`：
  - `data.command_id`: 建议携带；若携带则原样回指
- `sys.ack` / `sys.nack`：
  - `data.type`: 原始命令类型
  - `data.command_id`: 若原命令携带则回指
  - `data.reason`: 仅 `sys.nack` 使用
- `evt.camera.state`：
  - 当前仅作为相机联调兼容扩展事件，不属于 `device_communication_protocol.md v0.1.5` 的硬件端主消息总表
- `ctrl.robot.state.set`：
  - 当前仅作为本地行为状态注入兼容扩展，不属于 `device_communication_protocol.md v0.1.5` 的硬件端主消息总表

### FZ-05 WebSocket 音频协议
- 二进制帧头冻结为 `WSPK` `14B`：
  - `magic(4)` + `frame_type(1)` + `flags(1)` + `seq(4)` + `payload_len(4)`
- 序号冻结：
  - 每个 `frame_type` 各自维护本地自增 `seq`
- flags 冻结：
  - `FIRST=0x01`
  - `LAST=0x02`
  - `KEYFRAME=0x04`
  - `FRAGMENT=0x08`
- 上行（设备 -> 服务端）：
  - 二进制帧：`WSPK` 帧头 + 裸 `PCM`
  - `frame_type = 1`
  - `payload`: `16-bit`，`16kHz`，`mono`
  - 当前发送周期：约 `60ms/帧`（典型 `1920 bytes`）
  - 首帧加 `FIRST`
  - 录音结束标记：发送 `LAST` 置位的零负载包
- 下行（服务端 -> 设备）：
  - 二进制帧：`WSPK` 帧头 + 裸 `PCM`
  - `frame_type = 1`
  - `payload`: `16-bit`，`24kHz`，`mono`
  - TTS 结束标记：发送 `LAST` 置位的最后一个音频包，不再使用 `"over"` / `tts_end`
  - 本地音效与云端 TTS 互斥；若 TTS 正在播放，状态脚本中的本地音效轨会被跳过

### FZ-06 WebSocket 图片/视频协议
- 以 `device_communication_protocol.md v0.1.5` 为唯一真源。
- 本地旧版 [SERVER_GATEWAY_CAMERA_MEDIA_PROTOCOL.md](D:\GithubRep\WatcheRobot-Firmware\firmware\s3\docs\SERVER_GATEWAY_CAMERA_MEDIA_PROTOCOL.md) 仅保留历史参考价值，不再作为冻结真源。
- 本轮冻结的核心结论：
  - 单帧图片 = `JPEG`
  - 连续视频 = `MJPEG` 风格的 `JPEG frame sequence`
  - camera media 二进制帧统一使用 `WSPK` `14B` 帧头
  - 当前协议不再使用 `stream_id`
- 单帧抓拍控制：
  - `{"type":"ctrl.camera.capture_image","code":0,"data":{"command_id":"cam-shot-001"}}`
- 视频流开始控制：
  - `{"type":"ctrl.camera.start_video","code":0,"data":{"command_id":"cam-start-001","fps":5}}`
- 视频流停止控制：
  - `{"type":"ctrl.camera.stop_video","code":0,"data":{"command_id":"cam-stop-001"}}`
- 受理成功响应：
  - `{"type":"sys.ack","code":0,"data":{"type":"ctrl.camera.start_video","command_id":"cam-start-001"}}`
- camera 状态兼容事件：
  - `{"type":"evt.camera.state","code":0,"data":{"action":"start_video","state":"started","fps":5,"message":"format=mjpeg"}}`
- 图片二进制帧：
  - `frame_type = 3`
  - `flags = FIRST | LAST | KEYFRAME`
  - `payload = 完整 JPEG`
- 视频二进制帧：
  - `frame_type = 2`
  - 普通帧：`flags = KEYFRAME`
  - 首帧：`flags = FIRST | KEYFRAME`
  - 结束包：`flags = LAST` 且 `payload = 0`
  - 每个二进制包负载都是一帧完整 `JPEG`
- 当前冻结约束：
  - 单帧和连续流都复用同一相机回调链路
  - 服务端必须按 `WSPK` 帧头解析 camera media
  - 服务端不能把它当 H.264/H.265 码流处理
  - 当前版本不承诺同一 `frame_type` 内的多路并发流

### FZ-07 执行约束（联调必须知晓）
- 舵机：
  - `X` 轴范围 `0..180`
  - `Y` 轴最终被机械保护钳位到配置区间（当前默认 `90..150`）
- 显示文本：
  - 终端显示层最终会截断到 `30` 字符并追加 `...`

## 4. 暂不冻结（Freeze Later）
- BLE 配网 UX / 安全增强（当前已支持基础 SSID/PASS 下发与连接）。
- OTA 真正写分区与校验切换流程（当前为 stub / nack 占位）。
- 更复杂的视频多路复用、分片与重传策略。
- SSCMA 推理结果对云侧的结构化上报协议。

## 5. 协议版本统一建议（本轮已对齐）
- 当前对外协议版本统一为：`Watcher-WS-Protocol v0.1.5`。
- 代码实现与服务端联调均应以 `v0.1.5` 为准。
- 当前 beta 固件版本标识：`0.1.0-beta`。
- 向后兼容要求（冻结）：
  - BLE 继续兼容旧文本舵机命令：`X:` / `Y:` / `SET_SERVO:` / `SERVO_MOVE:` / `PING`
  - `evt.ai.status` 继续接受历史状态词：`analyzing` / `completed` / `fail`

## 6. 联调验收最小用例（建议）
1. Discovery 成功：设备 30s 内拿到包含 `protocol_version=0.1.5` 的 `ANNOUNCE` 并建立 WS。
2. Hello 握手成功：设备首发 `sys.client.hello`，服务端回 `sys.ack(type=sys.client.hello)`，固件进入 `session_ready`。
3. Servo：`ctrl.servo.angle` 的 `x_deg/y_deg/duration_ms` 路径可执行，并持续上报 `evt.servo.position`。
4. AI 状态：`evt.ai.status` 的 `status / message / image_name / action_file / sound_file` 能被固件解析并落到本地行为状态资源。
5. 语音上行：持续发送 `14B WSPK + PCM`，结束后发送 `LAST` 音频包。
6. TTS 下行：收到 `14B WSPK + PCM` 播放，收到 `LAST` 音频包后正确收尾。
7. Capture single：收到 `ctrl.camera.capture_image` 后，设备返回 `sys.ack`，并上行 1 个 `frame_type=3`、`FIRST|LAST|KEYFRAME` 的 `WSPK + JPEG` 图片帧。
8. Capture stream：收到 `ctrl.camera.start_video` 后，设备返回 `sys.ack` 与兼容 `evt.camera.state(started)`，随后持续上行 `frame_type=2` 的 `WSPK + JPEG` 视频帧；收到 `ctrl.camera.stop_video` 后发送 `LAST` 结束包并上报兼容 `evt.camera.state(stopped)`。
9. BLE / 本地状态兼容：旧文本舵机命令与兼容 JSON `ctrl.robot.state.set` 都能正确执行。

## 7. 风险记录（不阻塞本次冻结）
- Discovery 与 WS 目前均未做链路鉴权/加密。
- Wi-Fi 配网当前仍依赖本地已保存 STA 配置或 BLE 下发，尚未加入更严格的凭据保护策略。
- 本地仍保留 `ctrl.robot.state.set` 与 `evt.camera.state` 兼容扩展；它们不是 `device_communication_protocol.md v0.1.5` 的硬件端主消息集合。
- 当前 camera media 走 `JPEG/MJPEG`，不是压缩视频码流；带宽与 CPU 成本都由服务端/网关承担解 JPEG。
- 当前板端刷机仍偶发无法自动进入下载模式，可能需要手动 `BOOT + RESET`。
