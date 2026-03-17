# Watcher 协处理器通信开发设计

> 目的：把 `HX6538 <-> ESP32-S3` bring-up、单帧抓拍、连续视频流、服务接入和后续联调路线收敛成一份可直接指导开发的设计文档。
>
> 范围：只覆盖板内协处理器通信、相机 HAL 与 camera service 的设计，不定义云侧 `WebSocket` 对外协议。
>
> 协议基线：所有线上格式以 `docs/COPROC_COMM_PROTOCOL.md` 为准；本文只回答“怎么开发”。

## 1. 目标与边界

### 1.1 目标

本分支的目标不是再发明一套图像协议，而是把当前仓库已经具备的 `SPI SSCMA` 能力真正接进相机模块：

1. 打通 `ESP32-S3 <-> HX6538`
2. 完成连接握手与设备/模型信息查询
3. 支持单帧抓拍
4. 支持连续视频帧获取
5. 把图像与推理结果整理成 HAL 和 service 能消费的统一内部数据结构

### 1.2 明确不做

当前设计刻意不把以下内容混进本轮实现：

- 不重写 `sscma_client` transport
- 不设计新的云侧视频协议
- 不把 `1080p30/720p60` 传感器理论能力写成当前交付目标
- 不在第一步就把所有场景塞进 `WebSocket`

### 1.3 与现有文档的关系

- `docs/COMM_PROTOCOL_FREEZE.md`：对外网络协议
- `docs/COPROC_COMM_PROTOCOL.md`：板内协处理器通信格式
- 本文：板内通信的实现路线与模块边界

## 2. 当前代码现实

### 2.1 已经存在、可复用的部分

板级初始化已经具备：

- `bsp_sscma_client_init()`
- `bsp_sscma_flasher_init()`
- `sscma_client_new_io_spi_bus()`
- `sscma_client_init()/request()/invoke()/get_info()/get_model()/get_sensor()`

这意味着 transport 和基础命令层不需要重写。

### 2.2 当前仍是 stub 的部分

当前仓库里与相机直接相关的业务层仍是 stub：

- `components/hal/hal_camera`
- `components/services/camera_service`

现状可以概括成：

```text
板级 SSCMA 可初始化
协议层可收发命令
上层相机 HAL / service 还没有真正接上
```

### 2.3 当前可依赖的事实

- runtime 视觉链路走 `SPI SSCMA`
- `INVOKE` 能发命令
- 结果负载已有统一解析辅助函数
- `image` 当前以字符串形式上来

### 2.4 当前不能假设的事实

- 不能假设图像已经是裸 JPEG bytes
- 不能假设 `times=0` 在当前板上一定稳定连续出流
- 不能假设当前输出帧尺寸一定固定
- 不能假设连续流时不会出现丢帧、超时或事件缺失

## 3. 推荐总体架构

推荐分层保持简单，避免 transport 与业务纠缠：

```text
sensecap-watcher
  -> sscma_client
  -> hal_camera
  -> camera_service
  -> optional ws router / upper app
```

职责划分：

| 模块 | 职责 | 不负责 |
| --- | --- | --- |
| `sensecap-watcher` | 板级引脚、SPI bus、client handle 初始化 | 图像解码、流控策略 |
| `sscma_client` | SPI transport、AT/JSON 路由、基础 payload 提取 | 业务级帧管理 |
| `hal_camera` | 协处理器会话、抓拍/流控、图像解码、结果归一化 | 云侧推送 |
| `camera_service` | 生命周期管理、订阅分发、统计与策略控制 | transport 细节 |

## 4. 开发分期

### Phase 0: 握手与健康检查

目标：

- `hal_camera_init()` 真正建立 SSCMA client
- 等待 `INIT@STAT`
- 读取 `ID/NAME/VER/MODEL/INFO/SENSOR`

输出：

- 一份稳定的初始化日志
- 一份当前模型与类别表缓存

退出标准：

- 硬件上电后，能稳定打印协处理器信息
- init 失败时不影响主系统继续启动

### Phase 1: 单帧抓拍

目标：

- `hal_camera_capture_once()` 可返回一帧图像
- 能拿到同帧推理结果

建议路径：

1. 注册 `on_response/on_event`
2. 发 `INVOKE`
3. 等待后续结果回包
4. 提取 `data.image`
5. 提取 `boxes/classes/points/keypoints`
6. 归一化成 HAL 内部 frame 对象

退出标准：

- 单次抓拍可在超时内成功
- 失败可明确区分是“未连上”“无图像”“解码失败”“超时”

### Phase 2: 连续视频流

目标：

- `hal_camera_start()` 与 `hal_camera_stop()` 真正可用
- 能持续输出 frame + inference

优先策略：

1. 先验证 `times=0` 连续模式是否在当前硬件稳定工作
2. 如果协处理器连续模式表现不稳定，退化为“单飞行 paced invoke loop”

不论采用哪条路径，统一要求：

- 任一时刻只允许一个 in-flight `INVOKE`
- 上一帧未完成前，不发下一次新请求

退出标准：

- 连续运行至少 60s 不锁死
- FPS、超时、丢帧、解码失败有统计

### Phase 3: camera_service 接入

目标：

- 由 `camera_service` 管理相机流生命周期
- 给上层留一个稳定订阅面

推荐行为：

- service 只依赖 `hal_camera`
- service 保存最新帧状态和统计
- service 管理 start/stop、模式切换和订阅者注册

退出标准：

- `start -> stop -> start` 可重复执行
- 上层不需要直接碰 `sscma_client`

### Phase 4: 与上层业务对接

目标：

- 为后续 `WebSocket` 或本地业务消费提供稳定内部接口

这里的重点不是立刻上传视频，而是先把本地 frame/inference 合同打稳。

## 5. 推荐数据流

### 5.1 单帧路径

```text
hal_camera_capture_once()
  -> sscma_client_invoke()
  -> 协处理器 response: INVOKE accepted
  -> 协处理器 event/response: result payload
  -> extract image + inference
  -> decode image string
  -> build frame object
  -> callback to consumer
```

### 5.2 连续流路径

```text
camera_service_start_stream()
  -> hal_camera_start()
  -> camera worker loop
  -> one in-flight invoke
  -> wait payload
  -> decode
  -> publish latest frame
  -> pace next invoke
```

### 5.3 推荐内部处理顺序

每次回包都按相同顺序处理：

1. 先判定 envelope 是否有效
2. 再判定是否为本次相机请求期待的结果
3. 先提取 inference，再提取 image
4. image decode 成功后再向上发布 frame
5. 若只有推理结果没有图像，则按策略记错或记为 partial result

## 6. 推荐内部数据结构

### 6.1 Frame 基础结构

建议 `hal_camera` 内部统一使用如下结构，再决定是否对外暴露同名或裁剪版：

```c
typedef enum {
    HAL_CAMERA_FORMAT_UNKNOWN = 0,
    HAL_CAMERA_FORMAT_JPEG,
} hal_camera_format_t;

typedef struct {
    uint32_t frame_id;
    uint32_t timestamp_ms;
    uint16_t width;
    uint16_t height;
    hal_camera_format_t format;
    uint8_t *data;
    size_t size;
    bool owns_buffer;
} hal_camera_frame_t;
```

设计原则：

- `width/height` 必须保留，不能在 service 层猜
- `data/size` 统一表达图像字节
- `owns_buffer` 明确内存归属

### 6.2 Inference 统一结构

建议把推理结果和图像拆开，但通过同一个 `frame_id` 关联：

```c
typedef struct {
    sscma_client_box_t *boxes;
    int num_boxes;
    sscma_client_class_t *classes;
    int num_classes;
    sscma_client_point_t *points;
    int num_points;
    sscma_client_keypoint_t *keypoints;
    int num_keypoints;
} hal_camera_inference_t;
```

如果后续需要跨模块传递，建议再包装一层拥有权结构，而不是把裸指针直接暴露给多个消费者。

### 6.3 Stream 状态结构

连续流至少需要一份状态对象：

```c
typedef struct {
    bool initialized;
    bool streaming;
    bool invoke_in_flight;
    uint32_t next_frame_id;
    uint32_t target_fps;
    uint32_t timeout_ms;
    uint32_t delivered_frames;
    uint32_t dropped_frames;
    uint32_t timeout_frames;
    uint32_t decode_failures;
    uint32_t empty_image_frames;
} hal_camera_stream_state_t;
```

这是后续判断系统是否稳定的最小观测面。

## 7. 图像与推理结果处理策略

### 7.1 图像处理

当前 `data.image` 是字符串，推荐策略：

1. 先把原始字符串完整保留到临时缓冲
2. 优先按 `Base64` 解码
3. 解码后检查是否符合 JPEG 魔数
4. 若不是 JPEG，再记录“编码未知”并返回错误

明确要求：

- 不要把 JSON 字符串直接当 `uint8_t *jpeg`
- 不要在 transport 层做图像业务判断

### 7.2 推理结果处理

推理结果按以下优先级处理：

1. `boxes`
2. `classes`
3. `points`
4. `keypoints`

原因不是协议优先级，而是多数视觉业务先用框和分类结果做联调，关键点通常是后续增强能力。

### 7.3 `target` 的解释方式

`target` 只是索引，不是最终类名。

必须走：

```text
target -> model info classes[] -> label string
```

如果 `INFO.classes` 不可用：

- 仍可上报 `target`
- 但日志中必须明确“label unresolved”

## 8. 连续流策略

### 8.1 单飞行请求

连续视频流必须采用单飞行请求模型：

- 同一时刻只允许一个 `INVOKE` 在等待结果
- 收到结果或超时后，才能进入下一轮

这样做的原因：

- 当前 SSCMA client 没有 `request_id`
- 回包匹配主要依赖 `name`
- 多个同名 `INVOKE` 并发会让上层状态机复杂度陡增

### 8.2 背压策略

推荐采用 `latest-frame-wins`：

- 如果消费者比相机慢，只保留最新帧
- 老帧丢弃，不累积长队列

原因：

- 视频流比“每一帧都不能丢”更看重低延迟
- ESP32-S3 内存不能承受无界帧队列

### 8.3 帧率策略

推荐目标：

- 第一版以 `10 fps` 级别为安全目标
- 先验证 `640x480` 或 `412x412` 实际能否稳定输出

不要把以下概念混为一谈：

- 传感器理论规格
- HX6538 模型工作分辨率，例如 `480x480`
- 当前 SSCMA 输出图像尺寸，例如 `412x412` / `640x480`

### 8.4 超时与恢复

每轮流控都必须显式超时：

- 发 `INVOKE` 后开始计时
- 超时则标记 `timeout_frames++`
- 清理 in-flight 状态
- 进入下一轮前视情况执行 flush/reset

若连续多次超时，建议策略：

1. 先做 `sscma_client_flush/reset`
2. 再必要时重建 client
3. 最后才上升到整个 camera service restart

## 9. 失败模式与处理口径

| 失败模式 | 现象 | 推荐处理 |
| --- | --- | --- |
| 协处理器未连接 | 无 `INIT@STAT` | init 失败，但不拖垮主系统 |
| `INVOKE` 被拒绝 | response `code != 0` | 立即失败，记录命令级错误 |
| 有结果但无 `image` | 只能拿到推理结果 | 记为 `empty_image_frames`，单帧失败，流模式可继续下一轮 |
| `image` 解码失败 | 不能形成 JPEG | 记为 `decode_failures` |
| SPI 超时 | 长时间拿不到 payload | 执行 timeout 恢复流程 |
| 事件缺失 | 只有 accepted response，无后续结果 | 按 timeout 处理 |
| 模型信息缺失 | `target` 无法映射 label | 允许继续，只是 label unresolved |

## 10. 模块 API 收敛建议

### 10.1 `hal_camera`

当前已有 API 可以保留，但行为要真正实现：

- `hal_camera_init()`
- `hal_camera_start(int fps, hal_camera_frame_cb_t cb, void *ctx)`
- `hal_camera_stop()`
- `hal_camera_capture_once(hal_camera_frame_cb_t cb, void *ctx)`
- `hal_camera_is_streaming()`

建议在内部新增但不一定立刻对外暴露：

- `hal_camera_get_stats()`
- `hal_camera_get_model_info()`
- `hal_camera_get_last_error()`

### 10.2 `camera_service`

推荐把它设计成“策略层”而不是“transport 转发层”：

- `camera_service_init()`
- `camera_service_start_stream(int fps)`
- `camera_service_stop_stream()`
- `camera_service_capture_once()`
- `camera_service_is_streaming()`

建议新增：

- `camera_service_register_frame_callback(...)`
- `camera_service_get_stats(...)`

如果当前阶段不想改公开头文件，也至少要在实现里预留内部扩展点。

## 11. 验证矩阵

### 11.1 握手验证

- 上电后收到 `INIT@STAT`
- `ID/NAME/VER/MODEL/INFO/SENSOR` 都能成功读取

### 11.2 单帧验证

- `capture_once` 能在超时内成功
- 成功拿到图像字符串
- 成功解码为 JPEG
- 成功提取至少一种 inference 结果

### 11.3 连续流验证

- 连续运行 60s 不锁死
- `start -> stop -> start` 正常
- 内存不持续增长
- 统计项持续更新

### 11.4 分辨率验证

- 能记录实际运行时 `width/height`
- 不把 `480x480` 直接误判为输出尺寸

### 11.5 帧率验证

- 能统计实际 delivered FPS
- 能统计 timeout、drop、decode failure

### 11.6 压力验证

- 消费者慢时不堆积无界内存
- 故障恢复后仍能继续出帧

## 12. 推荐实现顺序

推荐严格按这个顺序推进，不并行跳步：

1. 打通 `hal_camera_init()`
2. 加模型与传感器信息读取
3. 打通 `capture_once`
4. 打通图像字符串解码
5. 接入 inference 结果
6. 实现连续流 worker
7. 接入 `camera_service`
8. 最后再考虑 `WebSocket` 上行

原因很简单：

- 如果单帧都没跑通，连续流和上云调试只会放大问题
- 如果 frame/inference 合同不先稳定，上层接口会反复返工

## 13. 验收口径

本分支“协处理器通信开发可开工”的最低标准：

1. 文档能准确描述当前 transport 与 payload
2. 开发者能依据本文定位到代码入口
3. `hal_camera` 的实现目标和边界清楚
4. 连续流的流控、丢帧、超时、恢复策略已提前拍板
5. 后续实现时不需要再重新讨论“SPI 上到底传什么”和“图像/推理结果怎么组织”

## 14. 关键默认决策

为避免后续反复讨论，本文固定以下默认值：

1. 运行时主链路以 `SPI SSCMA` 为准
2. `hal_camera` 是协处理器通信的第一业务接入点
3. `camera_service` 是相机生命周期与分发策略层
4. 连续流采用单飞行请求模型
5. 背压采用 `latest-frame-wins`
6. 第一版目标是“稳定可用”，不是追求理论最高分辨率或最高帧率
