# WatcheRobot S3 BLE 通讯重构计划（v0.0.97dev）

> 状态: Draft
> 计划版本: `v0.0.97dev`
> 对齐参考: `Watcher-WS-Protocol v0.1.5`
> 范围: `单舵机控制 + 状态下放触发行为播放 + BLE 配网服务`

## 1. 计划目标

当前 BLE 已经承担了两类真实职责：

- 本地配网
- 本地控制

但现状消息面还是分裂的：

- 舵机控制走旧文本命令
- 配网走旧文本命令
- 行为状态走兼容 JSON

这次 BLE 重构计划的目标不是把 BLE 做成第二套完整主协议，而是把它收敛成一条最小、稳定、可维护的本地控制通道：

- 控制语义尽量向 WS 主协议靠拢
- BLE 只覆盖当前真正需要的最小能力
- 保留 BLE 配网
- 不引入舵机位置回传和复杂本地会话

## 2. 当前实现基线

当前 BLE 实现在 [ble_service.c](/D:/GithubRep/WatcheRobot-Firmware/firmware/s3/components/protocols/ble_service/src/ble_service.c)，主要能力是：

- 舵机旧文本命令
  - `X:`
  - `Y:`
  - `SET_SERVO:`
  - `SERVO_MOVE:`
- 配网旧文本命令
  - `WIFI_STATUS`
  - `WIFI_CONFIG:`
  - `WIFI_CLEAR`
- 兼容 JSON
  - `ctrl.robot.state.set`
- 通知通道
  - 当前通过 notify 返回文本结果和 Wi-Fi 状态

当前 GATT 载体已经固定：

| 项 | 值 |
|---|---|
| Service UUID | `0x00FF` |
| Characteristic UUID | `0xFF01` |

本轮计划继续复用这组 UUID，不改蓝牙外设标识和单特征模型。

## 3. 重构边界

### 3.1 本轮保留

- 单舵机控制
- 状态下放，对应行为播放
- BLE 配网服务

### 3.2 本轮明确不做

- 舵机位置回传
- 多阶段动作状态事件
- 音频、视频、图片二进制传输
- OTA
- 完整双轴同步控制编排
- BLE 本地完整握手 / 会话恢复机制

说明：

- 当前舵机是无反馈方案，所以 BLE 不定义 `evt.servo.position`
- BLE app 只能认为自己下发的是目标角度，不能把它当真实角度

## 4. 目标消息面

BLE 顶层消息结构统一收敛为：

```json
{
  "type": "ctrl.servo.angle",
  "code": 0,
  "data": {}
}
```

约束：

- `type` 必填
- `code` 建议固定 `0`
- `data` 按消息类型变化
- 控制类消息统一返回 `sys.ack` 或 `sys.nack`

### 4.1 舵机控制

目标消息：`ctrl.servo.angle`

这是 WS 主协议的 BLE 单轴子集。

字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `x_deg` | number | 二选一 | 控制 X 轴时填写 |
| `y_deg` | number | 二选一 | 控制 Y 轴时填写 |
| `duration_ms` | int | 否 | 动作时长 |

约束：

- `x_deg` 和 `y_deg` 只能出现一个
- 一次消息只控制一个轴
- `duration_ms` 缺省时沿用固件默认值

示例：

```json
{
  "type": "ctrl.servo.angle",
  "code": 0,
  "data": {
    "x_deg": 45,
    "duration_ms": 300
  }
}
```

### 4.2 状态下放

目标消息：`evt.ai.status`

BLE 侧这里不是在模拟云端完整 AI 流程，而是借用 WS 主协议的状态语义，直接触发本地显示和行为播放。

字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `status` | string | 是 | 状态名 |
| `message` | string | 否 | 显示文案 |
| `image_name` | string | 否 | 图片资源名 |
| `action_file` | string | 否 | 动作资源名 |
| `sound_file` | string | 否 | 音效资源名 |

执行原则：

- 优先按 `action_file`
- 其次按 `status`
- 最后按本地 fallback

示例：

```json
{
  "type": "evt.ai.status",
  "code": 0,
  "data": {
    "status": "thinking",
    "message": "正在思考",
    "action_file": "thinking"
  }
}
```

### 4.3 BLE 配网

目标消息：

- `cfg.wifi.set`
- `cfg.wifi.get`
- `cfg.wifi.clear`
- `evt.wifi.status`

`cfg.wifi.set` 示例：

```json
{
  "type": "cfg.wifi.set",
  "code": 0,
  "data": {
    "ssid": "MyWiFi",
    "password": "12345678"
  }
}
```

`evt.wifi.status` 示例：

```json
{
  "type": "evt.wifi.status",
  "code": 0,
  "data": {
    "status": "connected",
    "ssid": "MyWiFi",
    "ip": "192.168.1.100"
  }
}
```

## 5. 统一反馈

### 5.1 成功

```json
{
  "type": "sys.ack",
  "code": 0,
  "data": {
    "type": "ctrl.servo.angle"
  }
}
```

### 5.2 失败

```json
{
  "type": "sys.nack",
  "code": 400,
  "data": {
    "type": "ctrl.servo.angle",
    "reason": "invalid_payload"
  }
}
```

### 5.3 配网状态

BLE 配网结果通过 `evt.wifi.status` 回传，不再额外定义别的状态事件。

## 6. 代码落地策略

### Phase 1

- 在 BLE write 入口先支持 JSON 消息识别
- 保留旧文本命令兼容
- 新增 `ctrl.servo.angle` 的单轴解析
- 新增 `cfg.wifi.*` 到现有 `wifi_manager` 的映射
- 新增 `evt.ai.status` 到现有 `behavior_state_service` 的映射

### Phase 2

- 把 BLE 成功/失败反馈统一收敛到 `sys.ack / sys.nack`
- 把 Wi-Fi 连接回调统一映射成 `evt.wifi.status`
- 保留旧文本返回，但只作为兼容期能力

### Phase 3

- BLE app 侧完成迁移后，把旧文本命令标记为 deprecated
- 文档和实现都以 JSON 消息面作为主入口

## 7. 和主协议的对应关系

| BLE 消息 | 来源 | 说明 |
|---|---|---|
| `ctrl.servo.angle` | 复用 WS 主协议 | 但只保留单轴子集 |
| `evt.ai.status` | 复用 WS 主协议 | 用于本地状态下放 |
| `sys.ack` | 复用 WS 主协议 | 统一成功响应 |
| `sys.nack` | 复用 WS 主协议 | 统一失败响应 |
| `cfg.wifi.set/get/clear` | BLE 本地扩展 | 只属于本地配网 |
| `evt.wifi.status` | BLE 本地扩展 | 只属于本地配网状态 |

## 8. 最小验收口径

1. BLE app 能发 `ctrl.servo.angle`，设备正确执行单轴动作，并返回 `sys.ack / sys.nack`
2. BLE app 能发 `evt.ai.status`，设备正确触发本地显示与行为播放，并返回 `sys.ack / sys.nack`
3. BLE app 能发 `cfg.wifi.set/get/clear`，设备正确执行配网动作，并通过 `evt.wifi.status` 回传结果
4. 旧文本命令在兼容期内继续可用，不阻塞现有工具

## 9. 文档定位

这份文档是 `v0.0.97dev` 阶段的 BLE 重构计划，不是最终冻结协议。

冻结前还需要确认两件事：

- BLE app 是否接受“单轴 `ctrl.servo.angle` 子集”而不是继续用 `X:` / `Y:`
- 配网是否要完全切到 `cfg.wifi.*`，还是继续长期保留 `WIFI_*` 文本命令
