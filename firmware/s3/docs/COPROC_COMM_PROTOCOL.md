# Watcher 协处理器通信协议定义

> 目的：冻结当前工程中 `HX6538 <-> ESP32-S3` 协处理器通信链路的真实格式定义，给后续 HAL、相机服务、视频流和联调工具提供统一依据。
>
> 范围：只覆盖当前仓库内已经存在的板级定义、`sscma_client` 传输层和 AT/JSON 负载层。不把云侧 `WebSocket` 协议并入本文。
>
> 关系说明：对外网络协议仍以 `docs/COMM_PROTOCOL_FREEZE.md` 为基线；本文只负责板内协处理器通信。

## 1. Source of Truth

本文优先以本地源码为准，必要时补官方语义说明。

本地源码主依据：

- `components/sensecap-watcher/include/sensecap-watcher.h`
- `components/sensecap-watcher/sensecap-watcher.c`
- `components/sscma_client/include/sscma_client_commands.h`
- `components/sscma_client/include/sscma_client_types.h`
- `components/sscma_client/include/sscma_client_ops.h`
- `components/sscma_client/src/sscma_client_io_spi.c`
- `components/sscma_client/src/sscma_client_ops.c`

官方参考：

- Seeed Watcher Hardware Overview
- Seeed Watcher Software Framework
- SSCMA 文档与 AT 协议说明

本文所有字段分两类标注：

- `当前工程已验证`：能直接从本地源码确认
- `官方语义/本地未强校验`：协议语义有官方依据，但当前仓库没有做完整字段校验或消费

## 2. 硬件拓扑与职责

当前 Watcher 板上的视觉链路可抽象为：

```text
OV5647 camera
  -> Himax HX6538
  -> SPI SSCMA
  -> ESP32-S3
  -> HAL / camera_service / 上层业务
```

职责边界：

- `OV5647`：图像传感器
- `HX6538`：视觉 AI 协处理器，负责取图、推理、组织 SSCMA 输出
- `ESP32-S3`：负责 UI、Wi-Fi、服务编排、上层协议和业务控制

注意：

- 当前板上的运行时主通信路径是 `SPI SSCMA`
- `UART1(GPIO17/18 @ 921600)` 在板级代码里主要作为 flasher/维护通道保留，不是当前 runtime 图像/推理主通路

## 3. 板级参数

来自 `sensecap-watcher.h` 与 `sensecap-watcher.c` 的当前板级定义：

| 项目 | 当前值 | 说明 |
| --- | --- | --- |
| SPI Host | `SPI2_HOST` | 协处理器使用的 SPI 主机 |
| SPI SCLK | `GPIO4` | 与 SPI2 总线定义一致 |
| SPI MOSI | `GPIO5` | 与 SPI2 总线定义一致 |
| SPI MISO | `GPIO6` | 与 SPI2 总线定义一致 |
| SSCMA CS | `GPIO21` | 协处理器片选 |
| SSCMA SYNC | `IO Expander Pin 6` | 协处理器数据就绪同步信号 |
| SSCMA RST | `IO Expander Pin 7` | 协处理器复位控制 |
| SPI Clock | `12 MHz` | `BSP_SSCMA_CLIENT_SPI_CLK` |
| SPI Mode | `0` | `sscma_client_new_io_spi_bus()` 使用 |
| SPI Wait Delay | `2 ms` | SPI 传输前等待 |

运行时初始化路径：

1. `bsp_io_expander_init()`
2. `bsp_spi_bus_init()`
3. 先把 `SD Card CS` 拉高，避免与协处理器共享总线时冲突
4. `bsp_sscma_client_init()` 创建 `sscma_client_io_spi`
5. `sscma_client_new()` 创建 SSCMA client

## 4. 协议分层

当前链路不是“SPI 上直接跑 JPEG 或结构体”，而是三层叠加：

```text
Layer 3: JSON payload
Layer 2: SSCMA AT text framing
Layer 1: SPI transport packet
```

### 4.1 Layer 1: SPI transport

`sscma_client_io_spi.c` 定义了 transport 常量：

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `HEADER_LEN` | `4` | transport header 长度 |
| `MAX_PL_LEN` | `250` | 单次写入最大 payload |
| `CHECKSUM_LEN` | `2` | transport trailer 长度 |
| `PACKET_SIZE` | `256` | `4 + 250 + 2` |
| `MAX_RECIEVE_SIZE` | `4095` | 单次读取最大长度 |
| `FEATURE_TRANSPORT` | `0x10` | transport feature 标识 |
| `READ` | `0x01` | 读命令 |
| `WRITE` | `0x02` | 写命令 |
| `AVAILABLE` | `0x03` | 查询可读长度 |
| `START` | `0x04` | 当前工程未使用 |
| `STOP` | `0x05` | 当前工程未使用 |
| `RESET` | `0x06` | flush/reset |

当前工程使用的 transport 命令只有：

- `WRITE`
- `READ`
- `AVAILABLE`
- `RESET`

`START/STOP` 目前只是常量存在，代码路径未实际使用。

### 4.2 Layer 2: SSCMA AT

AT 命令 framing 由 `sscma_client_commands.h` 定义：

```text
Request : AT+<CMD>[=<ARGS>]\r\n
Reply   : \r{...json...}\n
```

固定分隔符：

- `CMD_PREFIX = "AT+"`
- `CMD_SUFFIX = "\r\n"`
- `RESPONSE_PREFIX = "\r{"`
- `RESPONSE_SUFFIX = "}\n"`

### 4.3 Layer 3: JSON payload

AT 层承载的响应/事件最终都会被解析成 JSON object。

顶层 envelope 约定如下：

```json
{
  "type": 0,
  "name": "INVOKE",
  "code": 0,
  "data": {}
}
```

字段说明：

- `type`
  - `0`: response
  - `1`: event
  - `2`: log
- `name`
  - 命令名或事件名，比如 `MODEL`、`INFO`、`INVOKE`、`INIT@STAT`
- `code`
  - 协处理器返回码，`0` 通常表示成功
- `data`
  - 具体业务负载

## 5. SPI transport 详细格式

### 5.1 WRITE 包格式

当前实现把任意上层 AT 文本按 `250 bytes` 分片，每片封成一个 256 字节 SPI transport 包：

```text
byte[0] = 0x10                  // FEATURE_TRANSPORT
byte[1] = 0x02                  // WRITE
byte[2] = payload_len_hi
byte[3] = payload_len_lo
byte[4 .. 4+len-1] = payload
byte[4+len] = 0xFF
byte[5+len] = 0xFF
remaining bytes = 0x00 padding
```

说明：

- 单包 payload 上限 `250 bytes`
- 整包固定按 `PACKET_SIZE = 256` 发送
- 当前代码写入 `0xFF 0xFF` trailer，但本地实现没有做 trailer 校验
- 因此这两个字节在当前工程中应视作 transport footer，而不是已被证明可用的 checksum 机制

### 5.2 AVAILABLE 包格式

读取前先走 `available()`：

1. 如果配置了 `SYNC GPIO`，先读 `SYNC`
2. 若 `SYNC == 0`，直接认为没有待读数据
3. 若 `SYNC == 1`，发 `AVAILABLE` 包查询可读长度

`AVAILABLE` 包格式：

```text
byte[0] = 0x10
byte[1] = 0x03
byte[2] = 0x00
byte[3] = 0x00
byte[4] = 0xFF
byte[5] = 0xFF
remaining bytes = 0x00 padding
```

随后再单独接收 `2 bytes` 长度：

```text
avail_len = (rx[0] << 8) | rx[1]
```

特殊值：

- 如果读取结果为 `0xFFFF`，当前工程把它归一化为 `0`

### 5.3 READ 包格式

读取分两步：

第一步，发 read request：

```text
byte[0] = 0x10
byte[1] = 0x01
byte[2] = read_len_hi
byte[3] = read_len_lo
byte[4] = 0xFF
byte[5] = 0xFF
remaining bytes = 0x00 padding
```

第二步，再单独发一次 SPI RX 事务，将真正的数据时钟出来。

大包读取规则：

- 单次 read 真实接收长度上限为 `4095`
- 更大的上层响应会自动分多轮读取

### 5.4 RESET 包格式

`flush()` 走 RESET 包：

```text
byte[0] = 0x10
byte[1] = 0x06
byte[2] = 0x00
byte[3] = 0x00
byte[4] = 0xFF
byte[5] = 0xFF
remaining bytes = 0x00 padding
```

### 5.5 SYNC 语义

当前板级实现中：

- `SYNC` 由 IO Expander 提供
- `SYNC = 0` 时，ESP32-S3 直接跳过读取
- `SYNC = 1` 时，ESP32-S3 才进入 `AVAILABLE -> READ`

也就是说，当前工程的 SPI 读取是：

```text
SYNC gate + AVAILABLE length probe + READ payload
```

而不是盲读。

## 6. AT 命令层定义

### 6.1 当前工程显式定义的命令

`sscma_client_commands.h` 里已经声明的 AT 命令包括：

- `ID`
- `NAME`
- `VER`
- `STAT`
- `BREAK`
- `RST`
- `INVOKE`
- `SAMPLE`
- `INFO`
- `TSCORE`
- `TIOU`
- `ALGOS`
- `MODELS`
- `MODEL`
- `SENSORS`
- `SENSOR`
- `ACTION`
- `LED`
- `OTA`

并不是所有命令都在当前业务里用到，但这些名字是 transport 和响应路由的合法 command name。

### 6.2 响应类型

当前工程识别三种包类型：

- `type=0`: response
- `type=1`: event
- `type=2`: log

路由行为：

- `response` 优先按 `name` 匹配等待中的 request
- 没有匹配项时，走通用 `on_response`
- `event` 走 `on_event`
- `log` 走 `on_log`

### 6.3 初始化事件

事件名 `INIT@STAT` 在当前工程中被视作“协处理器已上线/已初始化”的 connect 信号。

这意味着 bring-up 时至少要等待：

```json
{
  "type": 1,
  "name": "INIT@STAT",
  "code": 0,
  "data": { ... }
}
```

当前代码对 `data` 的具体 shape 没有强校验，核心是 `name` 包含 `INIT@STAT`。

## 7. 顶层 JSON 负载定义

### 7.1 `ID`

请求：

```text
AT+ID?\r\n
```

响应：

```json
{
  "type": 0,
  "name": "ID",
  "code": 0,
  "data": "<device-id-string>"
}
```

### 7.2 `NAME`

请求：

```text
AT+NAME?\r\n
```

响应：

```json
{
  "type": 0,
  "name": "NAME",
  "code": 0,
  "data": "<device-name-string>"
}
```

### 7.3 `VER`

请求：

```text
AT+VER?\r\n
```

响应：

```json
{
  "type": 0,
  "name": "VER",
  "code": 0,
  "data": {
    "hardware": "...",
    "software": "...",
    "at_api": "..."
  }
}
```

### 7.4 `MODEL`

请求：

```text
AT+MODEL?\r\n
```

响应：

```json
{
  "type": 0,
  "name": "MODEL",
  "code": 0,
  "data": {
    "id": 1
  }
}
```

### 7.5 `INFO`

请求：

```text
AT+INFO?\r\n
```

响应：

```json
{
  "type": 0,
  "name": "INFO",
  "code": 0,
  "data": {
    "info": "<base64(json)>"
  }
}
```

说明：

- `data.info` 本身不是模型 JSON，而是 `base64` 之后的字符串
- 当前工程会先 decode，再 parse 成模型元数据

当前本地解析兼容两种模型信息格式。

旧格式：

```json
{
  "uuid": "model-uuid",
  "name": "model-name",
  "version": "1.0.0",
  "url": "https://...",
  "checksum": "sha256...",
  "classes": ["person", "cat"]
}
```

新格式：

```json
{
  "model_id": "1",
  "model_name": "model-name",
  "version": "1.0.0",
  "url": "https://...",
  "checksum": "sha256...",
  "classes": ["person", "cat"]
}
```

`classes` 的作用很重要：

- 后续推理结果里的 `target` 是类目索引
- 类别名需要结合 `INFO` 才能正确解释

### 7.6 `SENSOR`

查询请求：

```text
AT+SENSOR?\r\n
```

设置请求：

```text
AT+SENSOR=<id>,<enable>,<opt_id>\r\n
```

读取响应中，当前工程会把 `data.sensor` 解析为：

```json
{
  "type": 0,
  "name": "SENSOR",
  "code": 0,
  "data": {
    "sensor": {
      "id": 0,
      "type": 0,
      "state": 1,
      "opt_id": 0,
      "opt_detail": "..."
    }
  }
}
```

本地映射结构：

- `id`
- `type`
- `state`
- `opt_id`
- `opt_detail`

## 8. `INVOKE` 定义

### 8.1 请求格式

当前本地 API 为：

```c
sscma_client_invoke(client, times, filter, show)
```

但实际发到线上的是：

```text
AT+INVOKE=<times>,<filter>,<result_only>\r\n
```

本地参数到线上参数的映射是：

- `times` -> 直接透传
- `filter=true` -> `1`
- `filter=false` -> `0`
- `show=true` -> `result_only=0`
- `show=false` -> `result_only=1`

因此：

- `show=true` 表示“要图像”
- `show=false` 表示“只要推理结果”

示例：

```text
sscma_client_invoke(client, 1, false, true)
-> AT+INVOKE=1,0,0\r\n
```

```text
sscma_client_invoke(client, 1, false, false)
-> AT+INVOKE=1,0,1\r\n
```

### 8.2 响应与事件

`INVOKE` 不是一次请求只回一个包，而是两阶段：

第一阶段，命令受理响应：

```json
{
  "type": 0,
  "name": "INVOKE",
  "code": 0
}
```

第二阶段，真正结果通过异步 `event` 或未匹配的 response 回来，核心 payload 放在 `data` 中。

这点对视频流实现非常关键：

- `response` 只表示“协处理器收到了命令”
- 真正的图像与推理结果，通常要靠后续异步回包消费

### 8.3 `times` 的当前解释

当前工程本地代码只负责把 `times` 原样传到线上，不在本地做额外语义判断。

按 SSCMA 官方语义：

- `times > 0`：执行指定次数
- `times = 0`：连续/保持打开

但对本仓库来说，只有“参数原样透传”是当前工程已验证事实；`times=0` 在本地硬件上的连续流稳定性仍待实测。

## 9. 推理结果负载定义

当前本地解析器支持五类结果：

- `boxes`
- `classes`
- `points`
- `keypoints`
- `image`

### 9.1 `boxes`

JSON 形状：

```json
{
  "data": {
    "boxes": [
      [x, y, w, h, score, target],
      [x, y, w, h, score, target]
    ]
  }
}
```

本地映射结构：

```c
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint8_t score;
    uint8_t target;
} sscma_client_box_t;
```

字段语义：

- `x, y`: 左上角坐标
- `w, h`: 宽高
- `score`: 置信度
- `target`: 类别索引

### 9.2 `classes`

JSON 形状：

```json
{
  "data": {
    "classes": [
      [score, target],
      [score, target]
    ]
  }
}
```

本地映射结构：

```c
typedef struct {
    uint8_t target;
    uint8_t score;
} sscma_client_class_t;
```

注意：

- 线上数组顺序是 `[score, target]`
- 本地结构体顺序是 `target, score`
- 解析器已经做了字段重排

### 9.3 `points`

JSON 形状：

```json
{
  "data": {
    "points": [
      [x, y, score, target],
      [x, y, score, target]
    ]
  }
}
```

本地映射结构：

```c
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t z;
    uint8_t score;
    uint8_t target;
} sscma_client_point_t;
```

说明：

- 线上只提供 `x, y, score, target`
- 本地结构体里有 `z`
- 当前实现把 `z` 固定补成 `0`

### 9.4 `keypoints`

JSON 形状：

```json
{
  "data": {
    "keypoints": [
      [
        [x, y, w, h, score, target],
        [
          [x, y, score, target],
          [x, y, score, target]
        ]
      ]
    ]
  }
}
```

解释：

- `keypoints[i][0]` 是该实例的目标框
- `keypoints[i][1]` 是该实例的关键点列表

本地映射结构：

```c
typedef struct {
    sscma_client_box_t box;
    uint8_t points_num;
    sscma_client_point_t points[SSCMA_CLIENT_MODEL_KEYPOINTS_MAX];
} sscma_client_keypoint_t;
```

### 9.5 `image`

JSON 形状：

```json
{
  "data": {
    "image": "<string>"
  }
}
```

这是当前工程里最需要谨慎处理的字段。

当前工程已验证事实：

- `image` 在 JSON 层是字符串
- 本地 `sscma_utils_fetch_image_from_reply()` 只把它复制为 `char *`
- 当前 `sscma_client` 层没有把它直接解码成 `JPEG bytes`

官方语义/本地未强校验：

- SSCMA 的 AT 协议是文本协议
- 图像通常会以编码字符串形式嵌入 JSON
- 实际业务实现应按“编码后的图像串”设计解码路径，优先尝试 `Base64 -> JPEG`

对后续 HAL 的直接含义是：

- 不能假设 `data.image` 已经是裸二进制 JPEG
- 需要在 `hal_camera` 或更高层完成字符串解码与格式识别

## 10. 当前工程的数据消费模型

### 10.1 request 匹配方式

当前 `sscma_client_request()` 会把命令字符串做一次归一化：

- 去掉 `AT+`
- 去掉 `\r`
- 去掉 `\n`
- 去掉 `=`

然后再用 `name` 去匹配回包。

这意味着：

- 当前协议没有显式 `request_id`
- 命令相关性主要依赖 `name`
- 同名异步操作的上层状态管理，应由 HAL 或 service 自行补齐

### 10.2 读取流程

读取过程为：

```text
check SYNC
  -> AVAILABLE
  -> READ
  -> find "\r{" ... "}\n"
  -> cJSON_Parse()
  -> route by type/name
```

### 10.3 无效包判定

当前实现会把以下情况视为无效 reply：

- JSON 解析失败
- 缺少 `type`
- 缺少 `name`

## 11. 当前工程已验证 vs 推断边界

### 11.1 当前工程已验证

- 板级连接使用 `SPI SSCMA`
- `CS/SYNC/RST/Clock` 参数值
- SPI transport 的包头、命令字、分包大小、读流程
- AT 命令 framing
- 顶层 JSON 的 `type/name/code/data`
- `INIT@STAT` 被视作 connect 事件
- `MODEL/INFO/SENSOR/INVOKE` 等命令的请求格式
- `boxes/classes/points/keypoints/image` 的 JSON 结构
- `INFO` 是 `base64(JSON)`
- `INVOKE` 第三个线上参数在当前封装里等价于 `result_only`

### 11.2 官方语义/本地未强校验

- `times=0` 的连续流行为在当前硬件上的稳定性
- `data.image` 的最终编码方式细节
- 任何当前仓库未显式解析的附加字段，比如 `width/height/perf` 等扩展信息

## 12. 对后续实现的约束

基于本文，后续 `hal_camera` / `camera_service` 实现必须遵守：

1. 运行时 transport 以 `SPI SSCMA` 为准，不另起第二套自定义 SPI 协议
2. `INVOKE` 的命令成功与结果到达必须分两步处理
3. 图像字段按“字符串负载”接入，不能直接当裸 JPEG 指针向上传递
4. `target` 类结果需要结合 `INFO.classes` 做最终语义解释
5. 不把传感器理论规格直接写成当前项目可交付能力

## 13. References

- `components/sensecap-watcher/include/sensecap-watcher.h`
- `components/sensecap-watcher/sensecap-watcher.c`
- `components/sscma_client/include/sscma_client_commands.h`
- `components/sscma_client/include/sscma_client_types.h`
- `components/sscma_client/include/sscma_client_ops.h`
- `components/sscma_client/src/sscma_client_io_spi.c`
- `components/sscma_client/src/sscma_client_ops.c`
- Seeed Watcher Hardware Overview
- Seeed Watcher Software Framework
- SSCMA 文档与 AT 协议说明
