# WatcheRobot S3 BLE GATT 协议文档（当前实现）

> 状态: Implemented
> 更新时间: `2026-04-01`
> 适用固件分支: `codex/ble-communication-mainline-0.1.0`
> 建议 APP 对齐方式: `JSON over single GATT characteristic`

## 1. 作用范围

当前 BLE 只承担 3 类本地能力：

- 单舵机控制
- AI 状态下放，触发本地行为播放
- Wi-Fi 配网和 Wi-Fi 状态读取

当前实现补充行为：

- 设备启动后，BLE 本地控制不依赖 Wi-Fi 或 WebSocket 先连接成功
- BLE 已连接期间，会暂停 Wi-Fi / WS 后台链路，避免云端重试抢占本地控制
- BLE 断开后，如果设备已经保存过 Wi-Fi 凭据，会自动恢复 Wi-Fi / WS 后台链路

当前 BLE 明确不做：

- 舵机角度回传
- 双轴同步编排
- 音频、图片、视频传输
- OTA
- 完整 WS 会话握手

## 2. GATT 载体

当前 BLE 使用单服务、单特征模型。

| 项 | 值 |
|---|---|
| Peripheral Name | `CONFIG_WATCHER_BLE_DEVICE_NAME` |
| Service UUID | `0x00FF` |
| Characteristic UUID | `0xFF01` |
| Characteristic Property | `READ` + `WRITE` + `NOTIFY` |
| MTU | 目标 `247` |

说明：

- 所有命令和响应都走同一个 Characteristic
- 固件会把最近一次响应缓存到 Characteristic value 中，所以 APP 也可以在写入后主动 `read`

## 3. 传输规则

### 3.1 推荐模式

APP 端推荐统一使用 JSON 消息，格式如下：

```json
{
  "type": "ctrl.servo.angle",
  "data": {}
}
```

约束：

- `type` 必填
- `data` 必填，且必须是 object
- `code` 不是必填字段，BLE 当前实现不会依赖它
- `command_id` 如果需要透传，请放在 `data.command_id`

### 3.2 模式识别

固件会根据写入内容自动判断协议模式：

- 去掉前导空白后，如果首字符是 `{`，按 JSON 模式处理
- 否则按 legacy 文本命令处理

一次连接里可以混用，但不建议 APP 这样做。

### 3.3 返回通道

当前返回通道分两类：

1. 同步返回

- 如果 APP 使用 `write with response`
- 固件会把响应直接放进 GATT Write Response 里返回

2. 异步返回

- 如果 APP 开启了 Characteristic 的 CCCD
- 固件会通过 `notify` 推送异步结果和 Wi-Fi 状态

补充：

- 即使没开 notify，固件也会把最近一次响应缓存到 Characteristic value
- 所以 APP 也可以在写入之后再做一次 `read`

### 3.4 本地 UI 空闲提示

当前固件空闲态会根据链路状态自动切换文案：

- `Ready!`
  - 已具备云端会话
- `BLE Ready`
  - BLE 本地可用，但当前不处于“BLE 已连接且无 Wi-Fi”的提醒态
- `BLE Ready but no WiFi`
  - BLE 已连接且当前没有 Wi-Fi，使用告警红字显示

说明：

- 这是设备本地 UI 行为，不是 BLE 回包协议字段
- APP 不需要主动下发这条提示，只需要按正常 BLE 控制和配网流程工作

## 4. 推荐 JSON 协议

## 4.1 `ctrl.servo.angle`

用途：

- 单次控制一个舵机轴

字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `x_deg` | number | 二选一 | 控制 X 轴 |
| `y_deg` | number | 二选一 | 控制 Y 轴 |
| `duration_ms` | int | 否 | 动作时长，默认取固件配置值 |
| `command_id` | string | 否 | 原样回传到 ACK/NACK |

约束：

- `x_deg` 和 `y_deg` 必须且只能出现一个
- 角度范围 `0..180`
- `duration_ms` 范围 `0..5000`

示例：

```json
{
  "type": "ctrl.servo.angle",
  "data": {
    "x_deg": 45,
    "duration_ms": 300,
    "command_id": "cmd-001"
  }
}
```

成功返回：

```json
{
  "type": "sys.ack",
  "code": 0,
  "data": {
    "type": "ctrl.servo.angle",
    "command_id": "cmd-001"
  }
}
```

失败返回示例：

```json
{
  "type": "sys.nack",
  "code": 400,
  "data": {
    "type": "ctrl.servo.angle",
    "command_id": "cmd-001",
    "reason": "invalid_servo_payload"
  }
}
```

当前可能出现的 `reason`：

- `invalid_servo_payload`
- `invalid_duration_ms`
- `angle_out_of_range`
- `servo_move_failed`

## 4.2 `evt.ai.status`

用途：

- 从 APP 下放一个行为状态到设备端
- 触发本地显示、动作和音效播放

字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `status` | string | 建议 | 状态名 |
| `message` | string | 否 | 显示文案 |
| `image_name` | string | 否 | 图片资源名 |
| `action_file` | string | 否 | 动作资源名 |
| `sound_file` | string | 否 | 音效资源名 |
| `command_id` | string | 否 | 原样回传到 ACK/NACK |

执行规则：

- 固件会优先根据 `action_file`、`status`、fallback 规则映射到本地行为状态
- 如果提供 `message`，会一并下发到本地显示层

示例：

```json
{
  "type": "evt.ai.status",
  "data": {
    "status": "thinking",
    "message": "正在思考",
    "action_file": "thinking",
    "sound_file": "thinking",
    "command_id": "status-001"
  }
}
```

成功返回：

```json
{
  "type": "sys.ack",
  "code": 0,
  "data": {
    "type": "evt.ai.status",
    "command_id": "status-001"
  }
}
```

失败时可能出现的 `reason`：

- `unknown_ai_status`
- `ai_status_apply_failed`

## 4.3 `ctrl.robot.state.set`

用途：

- 兼容旧状态切换接口

字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `state_id` | string | 是 | 本地行为状态 ID |
| `command_id` | string | 否 | 原样回传 |

示例：

```json
{
  "type": "ctrl.robot.state.set",
  "data": {
    "state_id": "standby"
  }
}
```

失败时可能出现的 `reason`：

- `invalid_state_payload`
- `state_set_failed`

## 4.4 `cfg.wifi.set`

用途：

- 下发 Wi-Fi 凭据

字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `ssid` | string | 是 | Wi-Fi SSID |
| `password` | string | 是 | Wi-Fi 密码 |
| `command_id` | string | 否 | 原样回传 |

示例：

```json
{
  "type": "cfg.wifi.set",
  "data": {
    "ssid": "MyWiFi",
    "password": "12345678",
    "command_id": "wifi-set-001"
  }
}
```

失败时可能出现的 `reason`：

- `invalid_wifi_payload`
- `wifi_config_failed`

当前连接策略：

- 如果设备当前没有 BLE 连接，`cfg.wifi.set` 会保存凭据并立即启动 Wi-Fi 连接
- 如果设备当前正处于 BLE 会话中，`cfg.wifi.set` 会只保存凭据，不立即起连
- BLE 断开后，固件会自动恢复 Wi-Fi 后台连接流程

## 4.5 `cfg.wifi.get`

用途：

- 请求当前 Wi-Fi 状态

示例：

```json
{
  "type": "cfg.wifi.get",
  "data": {
    "command_id": "wifi-get-001"
  }
}
```

返回规则：

- 先返回 `sys.ack`
- 如果 APP 已开启 notify，随后异步推送 `evt.wifi.status`
- 如果 APP 没开 notify，建议写完之后主动 `read`

## 4.6 `cfg.wifi.clear`

用途：

- 清除已保存的 Wi-Fi 凭据

示例：

```json
{
  "type": "cfg.wifi.clear",
  "data": {
    "command_id": "wifi-clear-001"
  }
}
```

返回规则：

- 先返回 `sys.ack`
- 如果 APP 已开启 notify，随后异步推送 `evt.wifi.status`

失败时可能出现的 `reason`：

- `wifi_clear_failed`

## 4.7 `sys.ping`

示例：

```json
{
  "type": "sys.ping",
  "data": {}
}
```

返回：

```json
{
  "type": "sys.pong",
  "code": 0,
  "data": {}
}
```

## 4.8 `evt.wifi.status`

用途：

- 异步下发当前 Wi-Fi 连接状态

示例：

```json
{
  "type": "evt.wifi.status",
  "code": 0,
  "data": {
    "status": "connected",
    "ssid": "MyWiFi",
    "ip": "192.168.3.31"
  }
}
```

`status` 当前可能值：

- `connected`
- `connecting`
- `disconnected`
- `unconfigured`

## 5. 通用响应格式

### 5.1 成功

```json
{
  "type": "sys.ack",
  "code": 0,
  "data": {
    "type": "cfg.wifi.set",
    "command_id": "wifi-set-001"
  }
}
```

### 5.2 失败

```json
{
  "type": "sys.nack",
  "code": 400,
  "data": {
    "type": "cfg.wifi.set",
    "command_id": "wifi-set-001",
    "reason": "invalid_wifi_payload"
  }
}
```

### 5.3 不支持的消息

对于未实现的 JSON `type`，统一返回：

```json
{
  "type": "sys.nack",
  "code": 400,
  "data": {
    "type": "<original-type>",
    "reason": "unsupported_type"
  }
}
```

## 6. Legacy 文本协议

为了兼容旧 APP，当前固件仍然保留 legacy 文本命令。

建议：

- 新 APP 不要继续接入 legacy
- 只把 legacy 当兼容层

### 6.1 单舵机控制

支持：

- `X:<angle>[:duration]`
- `Y:<angle>[:duration]`
- `SET_SERVO:<servo_id>:<angle>[:duration]`
- `SERVO_MOVE:<servo_id>:<direction>`

约束：

- `servo_id=0` 表示 X 轴
- `servo_id=1` 表示 Y 轴
- `direction>0` 移到最大角度
- `direction<0` 移到最小角度
- `direction=0` 视为 no-op

成功返回通常为：

```text
OK
```

### 6.2 Wi-Fi 配网

支持：

- `WIFI_CONFIG:<json>`
- `WIFI_STATUS`
- `WIFI_CLEAR`

示例：

```text
WIFI_CONFIG:{"ssid":"MyWiFi","password":"12345678"}
```

返回示例：

- `WIFI_CONNECTING`
- `WIFI_CONNECTED:<ssid>:<ip>`
- `WIFI_DISCONNECTED:<ssid>`
- `WIFI_UNCONFIGURED`
- `WIFI_CLEARED`
- `WIFI_CONFIG_ERROR`
- `WIFI_CLEAR_ERROR`

### 6.3 心跳

支持：

```text
PING
```

返回：

```text
PONG
```

## 7. APP 端接入建议

APP 端推荐采用下面的最小接入策略：

1. 连接 BLE，发现服务 `0x00FF` 和特征 `0xFF01`
2. 优先使用 `write with response`
3. 如果需要接收 Wi-Fi 异步状态，打开 CCCD 订阅 notify
4. 所有业务命令统一走 JSON
5. 对于 `cfg.wifi.get` 和 `cfg.wifi.clear`，收到 `sys.ack` 后继续等待 `evt.wifi.status`
6. 如果没有开 notify，但又需要结果，写完后主动 `read` 特征值

## 8. 当前限制

- 舵机当前是无反馈方案，所以 BLE 不提供真实角度回传
- `ctrl.servo.angle` 当前一次只能控制一个轴
- BLE 没有实现 WS 那套完整的握手、会话和二进制帧体系
- `evt.ai.status` 是本地状态下放能力，不是完整的云端 AI 事件流
