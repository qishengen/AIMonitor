# AIMonitor 串口协议完整规范 (PROTOCOL)

> 版本: v1.0 ｜ 日期: 2026-08-08 ｜ 实现: `Code/AIMonitor/protocol.cpp`（固件端）+ `Code/host/protocol.py`（上位机端）

本文档为协议权威参考。数据来源：`protocol.cpp` 实际实现、`DESIGN.md`、`DEV_CONTEXT.md`。

---

## 1. 概述

| 项 | 值 |
|---|---|
| 传输介质 | 串口 UART（默认 921600 baud，可配） |
| 编码 | UTF-8 文本，一包一行 |
| 包分隔 | `\n`（LF） |
| 数据体 | 原始二进制（默认）或 base64 内嵌（`"enc":"b64"`）双模式 |
| JSON 能力 | 单层解析，value 仅支持字符串/整数，未知字段忽略 |
| 协议版本 | `proto = 1`，固件 `fw = "1.1.0"` |

接收端资源：行缓冲 256B、串口环形缓冲 8192B、数据批写 256B/批、收包不完整超时 1s（超时复位并回 `nack(2)`，防止卡死）。

---

## 2. 报文结构（三种形态）

### 2.1 普通命令（无数据体）

```
{"cmd":"ping","name":"x.anm","fps":24,"loop":1}\n
```

### 2.2 带数据体 — 原始二进制（默认，省体积）

JSON 头一行（含 `"size"` 声明长度），换行后紧跟 `size` 字节原始数据：

```
{"cmd":"show","seq":5,"size":1024}\n<1024 bytes raw XBM>
{"cmd":"up_chunk","off":0,"size":4096}\n<4096 bytes raw>
```

### 2.3 带数据体 — base64 内嵌（纯文本调试/传输）

数据以 base64 字符串放在 JSON 的 `"data"` 字段内，无需换行：

```
{"cmd":"show","seq":5,"size":1024,"enc":"b64","data":"AQID...=="}\n
```

> 固件端按 `"data":"` 标记流式切换到 base64 解码（`protocol.cpp:307`）。`enc` 字段本身并不参与判断，只要数据内嵌在 `"data"` 中即走 b64 路径。

---

## 3. PC → 设备 命令表

> 约定：**成功回包** 一列中的 `ack` 用 `{"resp":"ack"}` 表示；`state` 为 `{"resp":"state",...}`；`ready` 为 `{"resp":"ready",...}`。参数错一律回 `nack(2)`。

### 3.1 控制类

#### ping — 握手/探测

```json
{"cmd":"ping"}
```

- 参数：无
- 成功回包：`pong`（含固件版本/协议版本/FS 总容量/空闲）
- 失败回包：无

**demo**
```python
from protocol import Link, SerialTransport
import serial
mon_link = Link(SerialTransport(serial.Serial("COM5", 921600, timeout=0)))
print(mon_link.request({"cmd":"ping"}, expect=("pong",)))
# {"resp":"pong","fw":"1.1.0","proto":1,"fs_total":2981888,"fs_free":2800000}
```

---

#### status — 查询当前状态

```json
{"cmd":"status"}
```

- 参数：无
- 成功回包：`state`（state/name/seq）
- 失败回包：无

**demo**
```python
resp = mon_link.request({"cmd":"status"}, expect=("state",))
# {"resp":"state","state":"play","name":"demo.anm","seq":10}
```

---

#### stop — 停止播放

```json
{"cmd":"stop"}
```

- 参数：无
- 成功回包：`state`（固定 `state:"idle",name:"",seq:0`）
- 失败回包：无

**demo**
```python
mon_link.request({"cmd":"stop"}, expect=("state","ack"))
# {"resp":"state","state":"idle","name":"","seq":0}
```

---

### 3.2 管理类

#### list — 列出所有 .anm 动画

```json
{"cmd":"list"}
```

- 参数：无
- 成功回包：`list`（`anims` 数组）
- 失败回包：`nack(2)`（FS 枚举失败）

**demo**
```python
resp = mon_link.request({"cmd":"list"}, expect=("list",))
# {"resp":"list","anims":["demo.anm","boot.anm"]}
```

---

#### delete — 按名删除动画

```json
{"cmd":"delete","name":"demo.anm"}
```

- 参数：`name`（必填，文件名含 `.anm`）
- 成功回包：`ack`
- 失败回包：`nack(3)` 文件不存在；`nack(2)` 缺 name

**demo**
```python
resp = mon_link.request({"cmd":"delete","name":"demo.anm"})
# {"resp":"ack"}
```

---

#### play — 播放 LittleFS 中的动画

```json
{"cmd":"play","name":"demo.anm","fps":24,"loop":1}
```

- 参数：`name` 必填；`fps` 可选（0=用 .anm 文件头存储值）；`loop` 可选（非 0 循环）
- 成功回包：`state`（`state:"play"`，seq 从 0 开始）
- 失败回包：`nack(3)` 文件不存在；`nack(2)` 缺 name
- 其他：播放中周期上报 `progress`

**demo**
```python
mon_link.request(
    {"cmd":"play","name":"demo.anm","fps":24,"loop":1},
    expect=("state","ack"),
)
# {"resp":"state","state":"play","name":"demo.anm","seq":0}
```

---

### 3.3 实时显示类

#### show — 立即直显一帧

```json
{"cmd":"show","seq":5,"size":1024}\n<1024B 原始 XBM>
```

- 参数：`seq` 可选（帧序号）；`size` 必填且必须等于 `1024`
- 数据体：1 帧 128x64 XBM（1024B，LSB 位序）
- 成功回包：`ack`
- 失败回包：`nack(2)` size≠1024 或数据不完整
- 特性：完全独立，任意时刻可发（播放中/空闲/流中均生效），无帧率/总数，节奏由上位机控制

**demo**
```python
frame = bytes(1024)                     # 一帧全黑 XBM
mon_link.request({"cmd":"show","seq":5}, payload=frame, enc="bin")
# 等价 b64：mon_link.request({"cmd":"show","seq":5,"enc":"b64"},
#                           payload=frame, enc="b64")
# {"resp":"ack"}
```

---

#### stream_start — 动画流开始

```json
{"cmd":"stream_start","fps":24,"loop":1}
{"cmd":"stream_start","fps":24,"loop":0,"total":60}
```

- 参数：`fps` 可选（默认 24，固件夹到 1~60）；`loop` 可选（非 0 循环）；`total` 可选（给则收满自动结束，不给用 `stream_end` 显式结束）
- 成功回包：`state`（`state:"stream"`）
- 失败回包：无
- 特性：设备端按 fps 定时播放，帧早到排队（暂存 1-2 帧，超丢最新），晚到延时到点再播；不落盘

**demo**
```python
resp = mon_link.request(
    {"cmd":"stream_start","fps":24,"loop":1},
    expect=("state","ready"),
)
# {"resp":"state","state":"stream","name":"","seq":0}
```

---

#### frame — 流中发送一帧

```json
{"cmd":"frame","seq":0,"size":1024}\n<1024B 原始 XBM>
```

- 参数：`seq` 可选；`size` 必填且必须等于 `1024`
- 数据体：1 帧 XBM（1024B，LSB 位序）
- 成功回包：`ack`
- 失败回包：`nack(2)` size≠1024 或数据不完整

**demo**
```python
for i in range(60):
    frame = bytes(1024)                # 实际应替换为第 i 帧数据
    mon_link.request({"cmd":"frame","seq":i}, payload=frame)
    # {"resp":"ack"}
```

---

#### stream_end — 流显式结束

```json
{"cmd":"stream_end"}
```

- 参数：无
- 成功回包：`done`
- 失败回包：无
- 特性：仅当 stream_start 未给 `total` 时需要

**demo**
```python
resp = mon_link.request({"cmd":"stream_end"}, expect=("done","ack"))
# {"resp":"done"}
```

---

### 3.4 写入保存类

#### up_begin — 分块上传开始（断点续传）

```json
{"cmd":"up_begin","name":"demo.anm","size":61448}
```

- 参数：`name` 必填；`size` 必填，须 `>= 8` 且 `(size-8) % 1024 == 0`
- 成功回包：`ready`（含 `off` = 当前磁盘已写字节数，续传起点）
- 失败回包：`nack(2)` 参数错；`nack(5)` 存储满
- 特性：同名同尺寸保留原文件支持续传，否则重建

**demo**
```python
data = open("demo.anm","rb").read()   # 完整 .anm（含 8B 头）
resp = mon_link.request(
    {"cmd":"up_begin","name":"demo.anm","size":len(data)},
    expect=("ready",),
)
off = resp.get("off", 0)               # 断线后续传起点
# {"resp":"ready","off":0}
```

---

#### up_chunk — 分块写入数据

```json
{"cmd":"up_chunk","off":0,"size":4096}\n<4096B 原始>
```

- 参数：`off` 必填（>=0，写入偏移）；`size` 必填
- 数据体：任意字节数（<= 4096 建议）
- 成功回包：`ack`
- 失败回包：`nack(2)` 未先 up_begin、off 非法、或数据不完整
- 特性：off 支持断点续传；每块回 ack 后上位机发下一块

**demo**
```python
chunk = 4096
for off in range(off, len(data), chunk):
    piece = data[off:off+chunk]
    mon_link.request({"cmd":"up_chunk","off":off}, payload=piece)
    # {"resp":"ack"}
```

---

#### up_end — 上传结束 + CRC 校验落盘

```json
{"cmd":"up_end","crc":1234567890}
```

- 参数：`crc` 必填（整文件 CRC32，无符号 32 位）
- 成功回包：`done`
- 失败回包：`nack(4)` CRC 校验失败（自动删除坏文件，不落盘）；`nack(2)` 缺 crc 或未 up_begin
- 特性：CRC 校验通过才正式落盘

**demo**
```python
import zlib
crc = zlib.crc32(data) & 0xFFFFFFFF
resp = mon_link.request({"cmd":"up_end","crc":crc}, expect=("done",))
# {"resp":"done"}
```

---

#### transmit — 整动画一次传入，仅保存

```json
{"cmd":"transmit","name":"demo.anm","fps":24,"loop":1,"size":61440}\n<整 .anm 文件>
```

- 参数：`name` 必填；`size` 必填，须 `>= 8` 且 `(size-8) % 1024 == 0`；`fps`/`loop` 可选（存元数据，但播放用 play 命令指定或文件头 fps）
- 数据体：完整 `.anm` 文件（含 8B 头），原样落盘，不自动播放
- 成功回包：`done`
- 失败回包：`nack(2)` 参数错；`nack(5)` 存储满
- 特性：`fps`/`loop` 仅作记录，不参与播放控制

**demo**
```python
from anm_builder import build_anm
data = build_anm(frames, fps=24)       # frames: [1024B XBM,...]，LSB 位序
mon_link.request(
    {"cmd":"transmit","name":"demo.anm","fps":24,"loop":1,"size":len(data)},
    payload=data, expect=("done",),
)
# {"resp":"done"}
```

---

## 4. 设备 → PC 回包表

| 回包 | JSON | 含义 |
|---|---|---|
| ack | `{"resp":"ack"}` | 成功 |
| nack | `{"resp":"nack","code":n}` | 失败（见 §5） |
| ready | `{"resp":"ready","off":4096}` | 上传/流就绪，off 为续传起点 |
| done | `{"resp":"done"}` | 上传完成/流结束 |
| progress | `{"resp":"progress","seq":30,"fps":24}` | 播放进度（播放中周期上报） |
| state | `{"resp":"state","state":"play","name":"x.anm","seq":10}` | 状态查询/变更结果 |
| pong | `{"resp":"pong","fw":"1.1.0","proto":1,"fs_total":2981888,"fs_free":2800000}` | ping 响应 |
| list | `{"resp":"list","anims":["a.anm","b.anm"]}` | list 响应 |

> 回包同样是 `\n` 结尾的一行 JSON。上位机在等待某期望回包期间会**忽略** `progress`/`state` 等非期望回包（`protocol.py Link.request`）。

---

## 5. 错误码

| code | 含义 |
|---|---|
| 1 | 未知命令 |
| 2 | 参数错 / 数据不完整 |
| 3 | 文件不存在 |
| 4 | CRC 校验失败（坏文件不落盘） |
| 5 | 存储满 |

---

## 6. .anm 文件格式（LittleFS 存储）

```
偏移    字段
0-3     "ANM0" 魔数 (4B)
4       fps (1B)
5-6     frame_count 小端 (2B)
7       保留 (1B)
8..     每帧 1024B 裸 XBM，顺序拼接
```

- 128x64 单帧 = 128*64/8 = **1024B**
- 1 秒动画 @24fps = 24KB；LittleFS 可用约 2.5MB
- 帧 XBM 为 **LSB 位序**（字节 bit0 = 行最左像素），MSB 会左右镜像

---

## 7. 约束与注意事项

1. **LSB 位序**：所有帧数据（编辑器 `frameToDeviceBytes`、`anm_builder.to_xbm`、内置 `idle_anim.h`）必须是 LSB，否则镜像。
2. **JSON 单层解析**：value 只支持字符串/整数；命令参数中不要出现嵌套对象/数组（除 `list` 回包由固件端自行构造）。
3. **数据完整性**：带数据体命令若收到字节数与 `size` 不符，回 `nack(2)`；1s 无活动则复位状态机（防卡死）。
4. **show/frame 固定 1024B**：`size` 必须为 1024，否则 `nack(2)`。
5. **up_begin/transmit 尺寸约束**：`size >= 8` 且 `(size-8) % 1024 == 0`。
6. **stream 结束方式**：给 `total` 则收满自动结束；否则必须发 `stream_end`。
7. **transmit 不自动播放**：保存后需手动 `play`。
8. **上位机实现速查**：`Code/host/protocol.py`（`encode_packet`/`Link.request`）、`Code/host/aimonitor.py`（高层 API 已封装全部命令）。
