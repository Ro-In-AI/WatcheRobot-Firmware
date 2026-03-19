# Watcher 协处理器双通路设计

> 目的：评估并定义 `SSCMA control + PTL binary media` 在 Watcher 上的可落地方案。
>
> 结论先行：`AI 推理能力保留` 与 `二进制 SPI PTL 分发图像/推理结果` 可以并存，但不能直接照搬官方 `HX SPI master` 示例，正确方向是 `HX6538 slave PTL + ESP32-S3 master receiver + SSCMA control`。

## 1. 问题定义

当前 Watcher 已经跑通：

- `HX6538 -> SSCMA -> ESP32-S3`
- 单帧抓拍
- 连续视频流
- `640x480` 输出

但当前图像链路实际是：

```text
AT+INVOKE
-> HX6538 retrieve raw frame
-> HX6538 retrieve jpeg frame
-> HX6538 base64_encode(jpeg)
-> HX6538 JSON serialize(image + inference + perf)
-> SPI SSCMA 回到 ESP32
-> ESP32 base64_decode(jpeg)
-> ESP32 WebSocket 发送
```

这条路径的瓶颈已经实测明确：

- `HX6538 wait_image` 约 `162 ms`
- `ESP32 base64 decode` 约 `41 ms`
- `ESP32 ws_send` 约 `99 ms`
- `640x480 @ target 10 fps` 实际约 `3 fps`

因此，如果目标是：

1. 保留 HX6538 的 AI 能力
2. 提升图像输出效率
3. 为后续更高 fps 或更低延迟留出口

就需要把 `控制面` 和 `媒体面` 分开设计。

## 2. 当前两条官方通路

### 2.1 通路 A: SSCMA / AT / JSON

这是 Watcher 当前已经在用的路径。

Watcher 板级定义：

- `SPI2_HOST`
- `CS = GPIO21`
- `SYNC = IO Expander Pin 6`
- `CLK = 12 MHz`

当前 ESP32 侧 SPI 传输实现是命令式 transport：

- `FEATURE_TRANSPORT_CMD_WRITE`
- `FEATURE_TRANSPORT_CMD_READ`
- `FEATURE_TRANSPORT_CMD_AVAILABLE`
- `FEATURE_TRANSPORT_CMD_RESET`

这条路径的特点：

- 适合控制和状态查询
- 适合模型/设备信息读取
- 适合 `INVOKE` 触发
- 图像回传是 `base64 + JSON.image`

官方 HX6538 公开代码也明确如此：

- `startStream(kRefreshOnReturn)`
- `retrieveFrame(raw_frame, AUTO)`
- `retrieveFrame(frame, JPEG)`
- `base64_encode(frame)`
- `write("image", ...)`
- `setAlgorithmInput(_algorithm, raw_frame)`

因此这条路保留为 `control plane` 是合理的，但继续承担高效媒体面不合适。

### 2.2 通路 B: SPI PTL binary

官方 Himax 仓库里还有一套独立的 `spi_ptl` 二进制协议。

协议头固定为：

- `PTL_HEADER_LEN = 7`

结构为：

```text
2 bytes id
1 byte data_type
4 bytes data_size
payload
```

数据类型里已经直接支持：

- `DATA_TYPE_JPG`
- `DATA_TYPE_RAW_IMG`
- `DATA_TYPE_META_DATA`
- `DATA_TYPE_META_YOLOV8_OB_DATA`
- `DATA_TYPE_META_YOLOV8_POSE_DATA`
- `DATA_TYPE_META_PEOPLENET_DATA`

官方示例明确存在：

- 直接发 JPEG binary
- 直接发 AI metadata
- 直接发 RAW image

因此这条路天然适合做 `media plane`。

## 3. 能不能并存

答案是：**能，但要按双通路思维设计，不能把两套 framing 混成一套。**

### 3.1 可以并存的原因

`SSCMA` 和 `PTL` 的职责天然不同：

- `SSCMA`
  - 控制
  - 查询
  - 配置
  - 启停
  - fallback 单帧路径
- `PTL`
  - JPEG binary
  - RAW binary
  - AI metadata binary

AI 能力并不会因为媒体通路改成 `PTL` 而消失，因为推理仍然跑在 HX6538 上。变化只是：

- 推理结果不再必须塞进 JSON
- 图像不再必须走 base64

### 3.2 不能直接照搬的原因

当前 Watcher 现状是：

- `ESP32-S3 = SPI master`
- `HX6538 = SPI slave`

而很多官方 `spi_ptl` 示例用的是：

- `HX = SPI master`
- `外部接收器 = SPI slave`

这两者角色是反的。

如果直接搬 `hx_drv_spi_mst_protocol_write_sp(...)` 这种示例，和当前 Watcher 板级 SPI 主从关系冲突。

### 3.3 为什么现在判断仍然可行

因为官方仓库不只有 `spi_master_protocol.h`，还有：

- `spi_slave_protocol.h`

它公开了：

- `hx_drv_spi_slv_open()`
- `hx_drv_spi_slv_protocol_read_simple(...)`
- `hx_drv_spi_slv_protocol_write_simple_sp(...)`
- `hx_drv_spi_slv_protocol_write_simple_ex(...)`
- `hx_drv_spi_slv_protocol_write_busy_status()`
- `hx_drv_spi_slv_protocol_register_tx_cb(...)`
- `hx_drv_spi_slv_protocol_register_rx_cb(...)`

这说明官方协议本身支持：

- HX 当 slave
- 通过 PTL 读写 binary packet

这正好和当前 Watcher 的板级角色方向一致。

## 4. 推荐落地形态

推荐架构：

```text
ESP32-S3
  |-- SSCMA client (control plane)
  |-- PTL receiver (media plane)
  |
SPI master
  |
HX6538
  |-- SSCMA AT server
  |-- PTL slave writer
  |-- AI pipeline
```

### 4.1 控制面保留 SSCMA

继续使用现有命令：

- `INIT`
- `INFO`
- `MODEL`
- `SENSOR`
- `INVOKE`
- 未来可新增 `MEDIA.START` / `MEDIA.STOP` / `MEDIA.CONFIG`

作用：

- 管理会话
- 配置分辨率/FPS/质量 hint
- 启停 PTL 媒体输出
- 查询当前媒体模式

### 4.2 媒体面使用 PTL binary

图像和推理结果都走 PTL packet：

- `DATA_TYPE_JPG`
- `DATA_TYPE_RAW_IMG`
- `DATA_TYPE_META_*`

推荐优先组合：

1. `JPG + META_*`
2. 后续如果需要更复杂调试，再加 `RAW_IMG`

### 4.3 两种复用方式

#### 方式 A: 同一物理 SPI，模式切换

思路：

- 默认走 SSCMA transport
- 开始媒体流后切换到 PTL media mode
- 停流后切回 SSCMA

优点：

- 不需要额外硬件
- 复用现有板级 SPI

缺点：

- 驱动状态机复杂
- 不能在同一时刻任意混发 SSCMA 包和 PTL 包
- 出错恢复要设计得很稳

#### 方式 B: 同一物理 SPI，时隙/帧级复用

思路：

- 仍是同一条 SPI
- 但规定某些 transaction 是 SSCMA，某些 transaction 是 PTL
- 通过独立 header 或 mode latch 区分

优点：

- 理论上最灵活

缺点：

- 实现复杂度高
- 风险高
- 容易把当前稳定 SSCMA 通路破坏掉

#### 方式 C: 独立媒体通道

思路：

- SSCMA 保持当前 SPI 通路
- PTL 单独走另一条物理通路

优点：

- 协议最清晰
- 媒体与控制完全隔离

缺点：

- 当前 Watcher 板级不一定留了这条线
- 改动成本最高

**当前推荐：先按方式 A 评估。**

原因：

- 最符合当前板级现实
- 也是最可能在不改硬件的前提下落地的方案

## 5. 推荐时序

### 5.1 初始化

```text
ESP32 -> SSCMA INIT/INFO/MODEL/SENSOR
ESP32 -> SSCMA MEDIA.CONFIG(width,height,fps,meta_mask)
ESP32 -> SSCMA MEDIA.START(mode=ptl)
HX6538 -> switch media producer to PTL slave write
```

### 5.2 连续媒体输出

```text
ESP32 (SPI master)
  -> pull/read PTL packet
HX6538 (SPI slave)
  -> PTL DATA_TYPE_JPG
  -> PTL DATA_TYPE_META_*
  -> PTL DATA_TYPE_JPG
  -> PTL DATA_TYPE_META_*
```

推荐一帧的输出单位是：

1. 一包 `JPG`
2. 一包对应的 `META_*`

也可以反过来，但要固定。

### 5.3 停流

```text
ESP32 -> SSCMA MEDIA.STOP
HX6538 -> stop PTL output
ESP32 -> recover SSCMA mode
```

## 6. 需要改哪些模块

### 6.1 HX6538 侧

需要新增或改造：

1. `SSCMA AT` 新控制命令
   - `MEDIA.CONFIG`
   - `MEDIA.START`
   - `MEDIA.STOP`
   - 可选 `MEDIA.STATUS`

2. `PTL slave writer`
   - 初始化 slave PTL
   - 输出 `JPG`
   - 输出 `META_*`
   - 管理 busy / halt / tx_cb

3. `AI pipeline` 输出适配
   - 从当前 `JSON image + boxes/classes/...`
   - 改成 `binary JPG + binary META struct`

4. 流模式状态机
   - idle
   - sscma_only
   - ptl_streaming
   - error_recover

### 6.2 ESP32-S3 侧

需要新增或改造：

1. `hal_camera`
   - 保留当前 SSCMA init/control
   - 新增 `PTL read loop`
   - 新增 mode switch

2. `sscma_client`
   - 不建议直接强改现有 framing
   - 更合理是新增一个 `ptl_io_spi` 或 `hal_coproc_media_io`

3. `camera_service`
   - 从“每帧 invoke 抓图”
   - 改成“控制面 start 后，媒体面持续收包”

4. `ws_client`
   - 可以直接消费 PTL 输出后的 JPEG bytes
   - 不再需要 `base64 decode`

## 7. 为什么这条路会更快

因为它至少能砍掉三段确定存在的成本：

1. HX6538 侧 `base64_encode`
2. HX6538 侧 `JSON serialize(image)`
3. ESP32 侧 `base64_decode`

当前实测里，仅 ESP32 侧 decode 就约 `41 ms`。

而 HX6538 侧 `wait_image` 大头里，`base64 + JSON` 也很可能占了不小部分。

因此切到 PTL 后，合理预期是：

- 单帧延迟明显下降
- fps 明显高于当前 `~3 fps`

但要注意：

- 还不能承诺一定到 `10 fps`
- 因为 `sensor capture + jpeg encode + inference` 仍然在 HX 上

## 8. 当前最大风险

### 8.1 PTL 与 SSCMA 是否能在同一物理 SPI 上平滑切换

这是第一风险点。

需要进一步验证：

- 当前 SYNC/握手线是否足够表达 PTL mode
- 切回 SSCMA 后是否需要 reset
- 出错恢复是否会把 HX 卡死

### 8.2 META struct 兼容性

官方 PTL 里 metadata type 很多，但具体 struct 在不同模型场景中可能不一致。

所以我们需要：

- 选定一批当前真正用到的模型
- 固定这些模型的 metadata binary schema

### 8.3 现有 Watcher 出厂固件不包含这条媒体面

这意味着此方案不是“打开一个开关”，而是：

- 改 HX 固件
- 改 ESP32 固件
- 做双端联调

## 9. 建议的开发顺序

### Step 1

只做最小化 PoC：

- HX slave PTL 发 `DATA_TYPE_JPG`
- ESP32 master 收 `DATA_TYPE_JPG`
- 先不带 metadata

目标：

- 证明同一板级 SPI 上，`SSCMA control + PTL media` 可切换

### Step 2

补 `DATA_TYPE_META_*`

目标：

- 证明 AI 结果可以和图像一起发

### Step 3

把 `camera_service` 切到 PTL 流模式

目标：

- 不再每帧 `INVOKE`
- 形成真正的连续媒体接收链

### Step 4

重新做 `640x480 @ target 10 fps` 压测。

## 10. 当前建议

如果目标是：

- 保留 AI
- 同时让图像分发更快

那么当前最值得投入的方向就是：

**基于 `HX slave PTL + SSCMA control` 做 PoC。**

这是目前已知最有希望显著提升 fps 的路线；继续深挖当前 `AT + JSON + base64` 方案，收益会比较有限。
