# Watcher 服务端/网关 Camera Media 协议

> 状态: Draft Freeze Candidate
> 版本: 0.1.0
> 最后更新: 2026-03-18

## 1. 目标

本文只定义 `Watcher 硬件端 <-> 服务端/网关` 的相机相关协议，不重复定义音频、舵机、OTA。

本协议以 `E:/WinDownloads/device_communication_protocol(2).md` 为参考，但结合当前 Watcher 固件和 `HX6538 -> ESP32-S3 -> WebSocket` 的真实能力，对图像与视频传输做了最终裁剪与落地。

## 2. 为什么这样定

当前 Watcher 端的真实媒体能力不是 `H.264/H.265` 连续编码视频，而是：

- 协处理器返回 `JPEG` 图像
- 连续视频流本质是连续 `JPEG frame`
- 固件当前最稳的实现方式是 `paced one-shot invoke loop`

所以网关侧不应按“压缩视频码流”来设计，而应按：

- 单张图片: `JPEG image`
- 视频流: `MJPEG` 风格的 `JPEG frame sequence`

## 3. 总体原则

### 3.1 控制面

控制面使用 `JSON 文本帧`。

### 3.2 媒体面

媒体面使用 `二进制帧`，并在二进制帧前部携带统一 `WSPK` 帧头。

### 3.3 本轮只冻结 camera media

虽然参考协议里提出了“所有二进制帧统一帧头”，但为了不打断当前已跑通的音频链路，本轮只冻结：

- `image`
- `video`

音频仍沿用当前已冻结的裸 `PCM` 传输规则。

## 4. 文本控制消息

### 4.1 服务端/网关 -> Watcher

#### `ctrl.camera.video_config`

用于配置后续视频流参数。

```json
{
  "type": "ctrl.camera.video_config",
  "code": 0,
  "data": {
    "command_id": "cam-cfg-001",
    "width": 640,
    "height": 480,
    "fps": 5,
    "quality": 80
  }
}
```

说明：

- `command_id` 必填
- `width/height/fps/quality` 都是“期望值”
- Watcher 可以接受但不保证严格按该值输出

#### `ctrl.camera.capture_image`

触发单帧抓拍。

```json
{
  "type": "ctrl.camera.capture_image",
  "code": 0,
  "data": {
    "command_id": "cam-shot-001"
  }
}
```

#### `ctrl.camera.start_video`

开始视频流。

```json
{
  "type": "ctrl.camera.start_video",
  "code": 0,
  "data": {
    "command_id": "cam-start-001",
    "fps": 5
  }
}
```

说明：

- `fps` 可选
- 当前 Watcher 固件建议工作区间 `1..10`

#### `ctrl.camera.stop_video`

停止视频流。

```json
{
  "type": "ctrl.camera.stop_video",
  "code": 0,
  "data": {
    "command_id": "cam-stop-001"
  }
}
```

### 4.2 Watcher -> 服务端/网关

#### `sys.ack`

表示命令被接受并进入执行。

```json
{
  "type": "sys.ack",
  "code": 0,
  "data": {
    "command_id": "cam-start-001",
    "command_type": "ctrl.camera.start_video",
    "stream_id": 12
  }
}
```

约束：

- `sys.ack` 表示“命令受理成功”，不等同于媒体数据一定已经全部送达
- 当命令会产生媒体流时，`stream_id` 必须返回

#### `sys.nack`

表示命令在入口处被拒绝。

```json
{
  "type": "sys.nack",
  "code": 1,
  "data": {
    "command_id": "cam-start-001",
    "command_type": "ctrl.camera.start_video",
    "reason": "already_streaming"
  }
}
```

#### `evt.camera.state`

表示 camera 状态变化。

```json
{
  "type": "evt.camera.state",
  "code": 0,
  "data": {
    "action": "start_video",
    "state": "started",
    "stream_id": 12,
    "fps": 5,
    "message": "format=mjpeg"
  }
}
```

当前冻结的 `action/state` 组合：

| action | state | 说明 |
|---|---|---|
| `video_config` | `accepted` | 参数已接受 |
| `capture_image` | `completed` | 单帧抓拍已完成 |
| `capture_image` | `error` | 单帧抓拍失败 |
| `start_video` | `started` | 视频流已开始 |
| `start_video` | `error` | 视频流启动失败 |
| `stop_video` | `stopped` | 视频流已停止 |
| `stop_video` | `error` | 视频流停止失败 |

## 5. 二进制帧头

### 5.1 固定头格式

所有 camera media 二进制帧都使用以下头：

```text
+----------------------+----------------------+
| Binary Header (16B)  | Payload (N bytes)    |
+----------------------+----------------------+
```

字段定义：

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|---|---:|---|---|---|
| `0` | `4` | ASCII | `magic` | 固定 `WSPK` |
| `4` | `1` | uint8 | `frame_type` | `2=video`, `3=image` |
| `5` | `1` | uint8 | `flags` | 标志位 |
| `6` | `2` | uint16 LE | `stream_id` | 当前媒体流 ID |
| `8` | `4` | uint32 LE | `seq` | 包序号 |
| `12` | `4` | uint32 LE | `payload_len` | 负载长度 |

约束：

- 所有多字节整数字段都使用 `LE`
- 接收端必须先校验 `magic == "WSPK"`
- 当前 firmware 不做 camera media 分片

### 5.2 `frame_type`

| 值 | 含义 |
|---|---|
| `2` | 视频帧 |
| `3` | 图片帧 |

### 5.3 `flags`

| bit | 含义 |
|---|---|
| `bit0` | 首帧 |
| `bit1` | 末帧 |
| `bit2` | 关键帧 |
| `bit3` | 分片帧 |

当前冻结规则：

- `image` 固定为 `首帧 + 末帧 + 关键帧`
- `video` 的每个 `JPEG` 都是帧内编码，所以 `关键帧` 恒为 `1`
- `video` 的第一帧额外设置 `首帧`
- `video` 停止时发送一个 `payload_len = 0` 且 `末帧 = 1` 的结束包
- 当前不使用 `分片帧`

## 6. 图片传输定义

### 6.1 编码格式

- 负载格式固定为 `JPEG`
- `frame_type = 3`

### 6.2 一次完整图片抓拍

Watcher 在收到 `ctrl.camera.capture_image` 后：

1. 返回 `sys.ack`
2. 抓拍成功后发送 `frame_type = 3` 的二进制帧
3. 发送 `evt.camera.state(action=capture_image, state=completed)`

### 6.3 图片帧约束

- 一张图对应一个独立 `stream_id`
- `seq` 固定为 `1`
- `flags = first | last | keyframe`
- `payload` 为完整 `JPEG bytes`

## 7. 视频传输定义

### 7.1 编码格式

- 视频流的每一帧都是一个 `JPEG`
- 网关侧应按 `MJPEG` 方式消费
- `frame_type = 2`

### 7.2 一次完整视频会话

Watcher 在收到 `ctrl.camera.start_video` 后：

1. 返回 `sys.ack`
2. 返回 `evt.camera.state(action=start_video, state=started, stream_id=...)`
3. 持续发送 `frame_type = 2` 的二进制帧，每帧负载是一个完整 `JPEG`
4. 收到 `ctrl.camera.stop_video` 后，发送零负载结束包
5. 返回 `evt.camera.state(action=stop_video, state=stopped, stream_id=...)`

### 7.3 视频帧约束

- 同一视频会话保持同一个 `stream_id`
- `seq` 从 `1` 开始递增
- 每个二进制包都代表一帧完整 `JPEG`
- 当前不拆分跨 WebSocket 包

### 7.4 视频结束包

结束包定义：

- `frame_type = 2`
- `stream_id = 当前视频流 stream_id`
- `seq = 最后一个序号 + 1`
- `payload_len = 0`
- `flags.bit1 = 1`

网关看到该包后，应结束当前 `stream_id` 对应的视频会话。

## 8. Watcher 端真实能力约束

这些不是“理想值”，而是服务端设计时要接受的现实约束：

- 当前视频链路是 `JPEG/MJPEG`，不是 H.264
- 当前稳定目标帧率按 `10 fps class` 设计
- 当前实际输出分辨率以运行时 JPEG 解码结果为准
- 服务端不要假设配置下发的 `width/height` 一定被严格满足
- 当前 `quality` 字段在 firmware 中属于预留/建议项，不应作为强约束

当前已知应兼容的图像尺寸范围：

- `640x480`
- `412x412`

## 9. 网关接收端实现要求

### 9.1 文本面

网关必须能识别：

- `sys.ack`
- `sys.nack`
- `evt.camera.state`

### 9.2 二进制面

网关接收 camera media 时必须：

1. 先读 `16B` 头
2. 校验 `magic`
3. 根据 `frame_type` 路由到 `video/image`
4. 根据 `stream_id + seq` 做顺序校验
5. 直接把 `payload` 当作 `JPEG bytes`

### 9.3 不要做的假设

网关不应假设：

- 一定先收到状态事件再收到媒体帧
- 一定先收到 ACK 再收到媒体帧
- 视频帧一定是固定宽高
- 图片和视频会共用同一个 `stream_id`

## 10. 兼容策略

为了兼容当前较早版本的固件，服务端可以短期保留对以下旧消息的兼容：

- `capture`
- `video`

但从本协议版本开始，新开发统一使用：

- `ctrl.camera.video_config`
- `ctrl.camera.capture_image`
- `ctrl.camera.start_video`
- `ctrl.camera.stop_video`
- `sys.ack`
- `sys.nack`
- `evt.camera.state`
- `WSPK` camera media binary frames
