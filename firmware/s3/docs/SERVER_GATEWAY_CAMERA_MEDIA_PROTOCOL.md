# Watcher 图片流与视频流通讯协议

> 状态: Freeze Candidate
> 版本: 0.2.0
> 最后更新: 2026-03-18
> 适用对象: `Watcher 固件`、`本地网关`、`服务端媒体接收模块`

## 1. 文档目标

这份文档只讲 `图片流` 和 `视频流` 的通讯协议，目的是给服务端/网关同事一个可以直接实现的单一依据。

本文覆盖：

- 控制消息
- `WSPK` 二进制帧头
- 单张图片上传
- 连续视频上传
- 服务端接收规则
- 当前实机验证结果

本文不覆盖：

- 音频协议
- 舵机协议
- OTA 协议
- BLE 协议

## 2. 总体结论

Watcher 当前相机媒体链路不是 `H.264/H.265`，而是：

- 单张图片: `JPEG`
- 连续视频: `MJPEG`，即“连续的 JPEG 帧”

所以服务端/网关必须按下面的方式处理：

- 控制面: `WebSocket JSON 文本帧`
- 媒体面: `WebSocket 二进制帧`
- 二进制帧统一带 `WSPK` 16 字节头

## 3. 当前已实机验证的行为

这份协议不是纸面设计，下面这些行为已经在当前 Watcher 固件上跑通：

- UDP 发现: `DISCOVER -> ANNOUNCE`
- WebSocket 建连
- 单帧抓拍
- 连续视频流开始/停止
- `sys.ack`
- `evt.camera.state`
- `WSPK` 图片帧
- `WSPK` 视频帧
- 视频结束包

当前实测结果：

- 当前稳定视频请求值: `fps=5`
- 实测 5 秒约收到 `26` 帧，约 `5.2 fps`
- 当前实测 JPEG 解码分辨率: `240x240`
- JPEG 负载后面存在 `32~35` 字节 `0x00` 尾部 padding

最后一条很重要：

- 服务端不能要求 `payload` 必须刚好以 `FFD9` 结束
- 正确做法是: 找到最后一个 `FFD9`，把后面的 `0x00` padding 截掉再解码 JPEG

## 4. 通道模型

### 4.1 控制面

控制消息全部通过 `WebSocket 文本帧` 发送，格式是 JSON：

```json
{
  "type": "ctrl.camera.capture_image",
  "code": 0,
  "data": {}
}
```

### 4.2 媒体面

媒体数据全部通过 `WebSocket 二进制帧` 发送，格式是：

```text
WSPK Header (16 bytes) + Payload (N bytes)
```

其中：

- 图片 `payload` = 单张 JPEG
- 视频 `payload` = 单帧 JPEG

## 5. 文本控制协议

## 5.1 服务端/网关 -> Watcher

### `ctrl.camera.video_config`

用于配置后续视频流期望参数。

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

字段说明：

- `command_id`: 必填
- `width`: 可选，期望宽度
- `height`: 可选，期望高度
- `fps`: 可选，期望帧率
- `quality`: 可选，建议 JPEG 质量

注意：

- 这些值都是“期望值”，不是强保证
- 当前 Watcher 会接受配置，但运行时输出分辨率要以实际 JPEG 解码结果为准

### `ctrl.camera.capture_image`

触发单张图片抓拍。

```json
{
  "type": "ctrl.camera.capture_image",
  "code": 0,
  "data": {
    "command_id": "cam-shot-001"
  }
}
```

### `ctrl.camera.start_video`

开始连续视频流。

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
- 当前建议工作区间：`1..10`

### `ctrl.camera.stop_video`

停止当前视频流。

```json
{
  "type": "ctrl.camera.stop_video",
  "code": 0,
  "data": {
    "command_id": "cam-stop-001"
  }
}
```

## 5.2 Watcher -> 服务端/网关

### `sys.ack`

表示命令已受理。

```json
{
  "type": "sys.ack",
  "code": 0,
  "data": {
    "command_id": "cam-start-001",
    "command_type": "ctrl.camera.start_video",
    "stream_id": 2,
    "message": "accepted"
  }
}
```

字段说明：

- `command_id`: 回指原命令
- `command_type`: 原始命令类型
- `stream_id`: 涉及图片/视频流时返回
- `message`: 当前常见值为 `"accepted"`，服务端可选解析

约束：

- `sys.ack` 仅表示“已受理”
- 不等于媒体一定已经全部发出

### `sys.nack`

表示命令被拒绝。

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

### `evt.camera.state`

表示图片/视频相关状态变化。

```json
{
  "type": "evt.camera.state",
  "code": 0,
  "data": {
    "action": "start_video",
    "state": "started",
    "stream_id": 2,
    "fps": 5,
    "message": "format=mjpeg"
  }
}
```

当前冻结的 `action/state`：

| action | state | 含义 |
|---|---|---|
| `video_config` | `accepted` | 视频参数已接受 |
| `capture_image` | `completed` | 单张图片抓拍完成 |
| `capture_image` | `error` | 单张图片抓拍失败 |
| `start_video` | `started` | 视频流已启动 |
| `start_video` | `error` | 视频流启动失败 |
| `stop_video` | `stopped` | 视频流已停止 |
| `stop_video` | `error` | 视频流停止失败 |

## 6. `WSPK` 二进制帧头

## 6.1 含义

`WSPK` 是图片流/视频流二进制包的 4 字节魔数标记。

可以简单理解为：

- `WebSocket Packet`
- 或 `Watcher Socket Packet`

真正重要的不是名字本身，而是：

- 只要看到前 4 个字节是 ASCII `"WSPK"`
- 服务端就知道这是 Watcher 的 camera media 二进制包

## 6.2 固定头格式

所有图片流/视频流二进制帧都使用固定 16 字节头：

```text
+----------------------+----------------------+
| Binary Header (16B)  | Payload (N bytes)    |
+----------------------+----------------------+
```

字段定义：

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|---|---:|---|---|---|
| `0` | `4` | ASCII | `magic` | 固定 `"WSPK"` |
| `4` | `1` | uint8 | `frame_type` | `2=video`, `3=image` |
| `5` | `1` | uint8 | `flags` | 标志位 |
| `6` | `2` | uint16 LE | `stream_id` | 媒体流 ID |
| `8` | `4` | uint32 LE | `seq` | 包序号 |
| `12` | `4` | uint32 LE | `payload_len` | 负载长度 |

规则：

- 所有多字节整数字段都使用 `LE`
- 服务端必须先校验 `magic == "WSPK"`
- 当前固件不做 camera media 分片

## 6.3 `frame_type`

| 值 | 含义 |
|---|---|
| `2` | 视频帧 |
| `3` | 图片帧 |

## 6.4 `flags`

| bit | 含义 |
|---|---|
| `bit0` | 首帧 |
| `bit1` | 末帧 |
| `bit2` | 关键帧 |
| `bit3` | 分片帧 |

当前实际使用规则：

- 图片帧：`FIRST | LAST | KEYFRAME`
- 视频第一帧：`FIRST | KEYFRAME`
- 视频中间帧：`KEYFRAME`
- 视频结束包：`LAST` 且 `payload_len=0`
- 当前不使用 `分片帧`

## 7. 图片流协议

## 7.1 图片编码

- `frame_type = 3`
- `payload = JPEG bytes`

## 7.2 单张图片完整交互

当前固件实测顺序：

1. 服务端发送 `ctrl.camera.capture_image`
2. 设备返回 `sys.ack`
3. 设备发送一帧 `WSPK image`
4. 设备发送 `evt.camera.state(action=capture_image, state=completed)`

实际示例：

```json
{"type":"ctrl.camera.capture_image","code":0,"data":{"command_id":"cam-shot-001"}}
```

```json
{"type":"sys.ack","code":0,"data":{"command_id":"cam-shot-001","command_type":"ctrl.camera.capture_image","stream_id":1,"message":"accepted"}}
```

然后接收一帧二进制：

- `magic = WSPK`
- `frame_type = 3`
- `stream_id = 1`
- `seq = 1`

最后收到：

```json
{"type":"evt.camera.state","code":0,"data":{"action":"capture_image","state":"completed","stream_id":1,"fps":0}}
```

## 7.3 图片帧约束

- 一张图对应一个独立 `stream_id`
- `seq` 固定为 `1`
- `flags = FIRST | LAST | KEYFRAME`
- `payload_len` 由头部给出
- `payload` 是 JPEG，允许后面有少量 `0x00` padding

## 8. 视频流协议

## 8.1 视频编码

- `frame_type = 2`
- 每个二进制包都是一帧完整 JPEG
- 服务端应按 `MJPEG` 消费

## 8.2 视频完整交互

当前固件实测顺序：

1. 服务端发送 `ctrl.camera.video_config`
2. 设备返回 `sys.ack`
3. 设备返回 `evt.camera.state(action=video_config, state=accepted)`
4. 服务端发送 `ctrl.camera.start_video`
5. 设备返回 `sys.ack`
6. 设备返回 `evt.camera.state(action=start_video, state=started)`
7. 设备持续发送 `WSPK video`
8. 服务端发送 `ctrl.camera.stop_video`
9. 设备返回 `sys.ack`
10. 设备发送 `LAST` 结束包
11. 设备返回 `evt.camera.state(action=stop_video, state=stopped)`

## 8.3 视频帧约束

- 同一视频会话保持同一个 `stream_id`
- `seq` 从 `1` 开始递增
- 每个 `payload` 都是一帧完整 JPEG
- 当前不做跨包分片

## 8.4 视频结束包

结束包定义：

- `frame_type = 2`
- `stream_id = 当前视频流 stream_id`
- `seq = 最后一个视频帧序号 + 1`
- `payload_len = 0`
- `flags = LAST`

服务端看到这个包后，应结束当前视频会话。

## 9. JPEG 负载处理规则

这一节是服务端实现最重要的坑点。

## 9.1 不要假设 payload 恰好等于 JPEG 文件

当前实测不是：

- `payload == 完整 JPEG 文件字节数组`

而是：

- `payload == JPEG bytes + trailing zero padding`

常见 padding 大小：

- `32`
- `33`
- `34`
- `35`

## 9.2 正确处理方式

服务端拿到 `payload` 后：

1. 校验是否以 `FFD8` 开头
2. 从尾部向前查找最后一个 `FFD9`
3. 将 `FFD9` 后面的所有字节丢弃
4. 用截断后的字节作为最终 JPEG 解码

伪代码：

```text
if payload[0:2] != FFD8:
    invalid

eoi = payload.rfind(FFD9)
if eoi < 0:
    invalid

jpeg = payload[:eoi+2]
decode(jpeg)
```

## 9.3 当前实测结果

实测保存后的 JPEG 可以正常打开，说明：

- 协议是对的
- 数据是合法 JPEG
- 只需要兼容尾部 padding

## 10. 分辨率与帧率约束

## 10.1 分辨率

服务端不要把 `video_config.width/height` 当成强保证。

当前规则应为：

- `video_config.width/height` 只是期望值
- 实际分辨率以 JPEG 解码结果为准

当前实机验证：

- 当前输出分辨率为 `240x240`

历史设计/兼容目标仍建议服务端保留对以下尺寸的兼容：

- `240x240`
- `412x412`
- `640x480`

## 10.2 帧率

当前服务端可以传 `fps`

```json
{"type":"ctrl.camera.start_video","code":0,"data":{"command_id":"cam-start-001","fps":5}}
```

当前建议：

- 设计工作区间：`1..10`
- 实测 `fps=5` 时，5 秒收到 `26` 帧，约 `5.2 fps`

## 11. 服务端接收实现要求

服务端接收模块至少要做到下面这些事情：

## 11.1 文本面

必须识别：

- `sys.ack`
- `sys.nack`
- `evt.camera.state`

## 11.2 二进制面

收到二进制帧时：

1. 读取前 `16` 字节头
2. 校验 `magic == "WSPK"`
3. 解析 `frame_type / flags / stream_id / seq / payload_len`
4. 读取 `payload_len` 指定的负载
5. 如果是 `image/video`，按 JPEG 处理
6. 按最后一个 `FFD9` 截断尾部 padding
7. 用截断后的 JPEG 做解码/转发/存储

## 11.3 会话管理

建议以 `(frame_type, stream_id)` 管理媒体会话。

图片流：

- 一张图一个 `stream_id`
- 收到一帧后即可结束该图片会话

视频流：

- 同一个 `stream_id` 对应一个视频会话
- `seq` 递增
- 收到 `payload_len=0 + LAST` 时结束该视频会话

## 11.4 不要做的假设

服务端不要假设：

- 配置宽高一定会被严格执行
- JPEG 没有尾部填充
- 图片和视频会复用同一个 `stream_id`
- 视频结束一定靠文本消息判断

正确做法是：

- 文本消息做控制与状态管理
- 二进制消息做真正媒体收发
- 视频结束优先以 `WSPK LAST` 结束包为准

## 12. 推荐联调样例

## 12.1 单图抓拍

下行：

```json
{"type":"ctrl.camera.capture_image","code":0,"data":{"command_id":"cam-shot-001"}}
```

预期上行：

1. `sys.ack`
2. 一帧 `WSPK image`
3. `evt.camera.state(completed)`

## 12.2 开始视频

下行：

```json
{"type":"ctrl.camera.video_config","code":0,"data":{"command_id":"cam-cfg-001","width":640,"height":480,"fps":5,"quality":80}}
```

```json
{"type":"ctrl.camera.start_video","code":0,"data":{"command_id":"cam-start-001","fps":5}}
```

预期上行：

1. `sys.ack(video_config)`
2. `evt.camera.state(video_config=accepted)`
3. `sys.ack(start_video)`
4. `evt.camera.state(start_video=started)`
5. 多帧 `WSPK video`

## 12.3 停止视频

下行：

```json
{"type":"ctrl.camera.stop_video","code":0,"data":{"command_id":"cam-stop-001"}}
```

预期上行：

1. `sys.ack(stop_video)`
2. 一帧 `payload_len=0` 的 `WSPK LAST`
3. `evt.camera.state(stop_video=stopped)`

## 13. 最终给服务端同事的实现结论

如果只保留一句最重要的话，可以这样说：

> Watcher 的图片流和视频流都不是 H.264，而是 `WSPK + JPEG/MJPEG`。  
> 图片是 `frame_type=3` 的单张 JPEG，视频是 `frame_type=2` 的连续 JPEG 帧。  
> JPEG 负载后面允许有少量 `0x00` padding，服务端必须按最后一个 `FFD9` 截断后再解码。
