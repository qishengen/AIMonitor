# AIMonitor 串口协议完整规范 (PROTOCOL)

> 版本: v1.2 ｜ 日期: 2026-08-21 ｜ 实现: `Code/AIMonitor/protocol.cpp`（协议层）+ `Code/AIMonitor/transport.cpp`（通信层）+ `Code/host/protocol.py`（上位机端）
> v1.2 变更：通信层/协议层分层重构（transport.cpp 管字节收发，protocol.cpp 管命令语义）；回包路由改为**命令应答回源 + 异步事件广播**；新增 `home`/`reset`/`factory`/`wifi_reconnect` 命令；Web 门户升级为控制面板（`/status` `/cmd`）；FW 升至 1.3.0。
> v1.1 变更：新增 WiFi TCP 通路（:8088）、Web 配网（:80）、`wifi`/`wifi_status` 命令、`ping` 回包增加 `ip` 字段；FW 升至 1.2.0。

本文档为协议权威参考。数据来源：`protocol.cpp` 实际实现、`DESIGN.md`、`DEV_CONTEXT.md`。

---

## 1. 概述

| 项 | 值 |
|---|---|
| 传输介质 | 串口 UART（默认 921600 baud，可配）+ **TCP Server（默认 :8088）**，双通道共用同一协议状态机 |
| 编码 | UTF-8 文本，一包一行 |
| 包分隔 | `\n`（LF） |
| 数据体 | 原始二进制（默认）或 base64 内嵌（`"enc":"b64"`）双模式 |
| JSON 能力 | 单层解析，value 仅支持字符串/整数，未知字段忽略 |
| 协议版本 | `proto = 1`，固件 `fw = "1.3.2"` |
| Web 控制面板 | HTTP :80（`GET /` 面板、`/status` 快照、`/cfg`、`/scan`；`POST /save`、`/cmd`），非协议端口，见 `webcfg.cpp` |

接收端资源：行缓冲 256B、串口环形缓冲 8192B、数据批写 256B/批、收包不完整超时 1s（超时复位并回 `nack(2)`，防止卡死）。

**分层架构（FW 1.3.0）**：`transport.cpp`（通信层）统一收串口/TCP/Web 字节并组装帧，`protocol.cpp`（协议层）只做命令语义。回包路由：
- **命令应答**（ack/nack/ready/done/state/list/pong/wifi）→ **只回请求来源**（串口命令回串口、TCP 命令回对应客户端、Web 操作回 HTTP 响应体）
- **异步事件**（progress/done 播放上报）→ **广播**到串口 + 所有 TCP 客户端

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
# {"resp":"pong","fw":"1.3.2","proto":1,"ip":"192.168.4.1","fs_total":2981888,"fs_free":2800000}
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

#### home — 播放内置主页动画

```json
{"cmd":"home"}
```

- 参数：无
- 成功回包：`state`（`state:"home"`，seq:0）
- 失败回包：无
- 特性：播放内置 64x64 主页动画（`idle_anim.h`，循环），直到 `stop`/`show`/`play`/`stream` 打断；Web 控制面板「主页动画」按钮即发此命令

**demo**
```python
mon_link.request({"cmd":"home"}, expect=("state",))
# {"resp":"state","state":"home","name":"","seq":0}
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

### 3.5 网络配置类

#### wifi — 配置/切换 WiFi 模式（持久化 + 即时生效）

```json
{"cmd":"wifi","mode":"sta","ssid":"MyWiFi","pass":"12345678"}
{"cmd":"wifi","mode":"ap+sta","port":9000}
```

- 参数：`mode`(ap|sta|ap+sta)、`ssid`、`pass`、`port`(1..65535) 均可选，未下发字段保持原配置
- 成功回包：`ack`（**先回包后应用**：应用配置会短暂断开当前 TCP 连接）
- 失败回包：`nack(2)` 参数错
- 特性：写入 LittleFS `/wifi.cfg` 持久化并即时生效；重启仍生效。`ap` 且 ssid 留空时热点名自动为 `AIMonitor-<chipid>`；STA 可配静态 IP（`/wifi.cfg` 手改 ip/gw/mask）。**AP 热点密码独立**（空=开放，≥8 位=WPA2，见 `ap_pass` 命令）；`pass` 仅用于 STA 连路由器，不作用于热点。客户端上限 8（`WiFi.softAP(...,max_connection=8)`）。**回包后延迟 ~100ms 应用**（`g_reapply_at`，串口/TCP/Web 各通道先收到 ack）。
- 同功能也可用 Web 配网完成：浏览器打开 `http://<设备IP>`（:80）

**demo**
```python
mon_link.request({"cmd":"wifi","mode":"sta","ssid":"MyWiFi","pass":"12345678"})
# {"resp":"ack"}
```

---

#### ap_pass — 设置/清除 AP 热点密码

```json
{"cmd":"ap_pass","pass":"12345678"}   # 设置（≥8 位，WPA2）
{"cmd":"ap_pass","pass":""}            # 清除（热点开放）
```

- 参数：`pass` 必填；空字符串=开放热点，≥8 位=启用 WPA2，长度 1~7 返回 `nack(2)`
- 成功回包：`ack`（先回包后延迟 ~100ms 应用）
- 失败回包：`nack(2)` 缺 pass 或长度 1~7
- 特性：持久化到 `/wifi.cfg` 并即时生效；与 STA 密码独立（STA 密码不作用于热点）；**串口/TCP/Web 三通道统一走协议分发**（Web 控制面板配网表单「AP 热点密码」字段同功能）；待机屏 `PASS` 行实时显示。

**demo**
```python
mon_link.request({"cmd":"ap_pass","pass":"12345678"})
# {"resp":"ack"}
```

---

#### wifi_status — 查询当前 WiFi 状态

```json
{"cmd":"wifi_status"}
```

- 参数：无
- 成功回包：`{"resp":"wifi","mode":"sta","ssid":"MyWiFi","ip":"192.168.1.50","port":8088,"clients":1,"sta":1}`
- 失败回包：无
- 字段：`mode` 当前模式、`ssid`（ap 模式留空=自动热点名）、`ip` 设备 IP、`port` TCP 端口、`clients` 已连接 TCP 客户端数、`sta` 是否已连上路由器

**demo**
```python
resp = mon_link.request({"cmd":"wifi_status"}, expect=("wifi",))
# {"resp":"wifi","mode":"ap+sta","ssid":"MyWiFi","ip":"192.168.1.50","port":8088,"clients":1,"sta":1}
```

---

### 3.6 设备管理类

#### wifi_reconnect — 用已存配置重新连接 WiFi

```json
{"cmd":"wifi_reconnect"}
```

- 参数：无
- 成功回包：`ack`（回包后延迟 ~100ms 再应用，保证 Web 请求方收到响应）
- 失败回包：无
- 特性：等价于重读 `/wifi.cfg` 并 `net_apply()`；Web 控制面板「重连WiFi」按钮即发此命令

**demo**
```python
mon_link.request({"cmd":"wifi_reconnect"})
# {"resp":"ack"}
```

---

#### reset — 重启设备

```json
{"cmd":"reset"}
```

- 参数：无
- 成功回包：`ack`（回包后延迟 ~300ms 再重启，保证响应发出）
- 失败回包：无

**demo**
```python
mon_link.request({"cmd":"reset"})
# {"resp":"ack"}  之后设备重启
```

---

#### factory — 恢复出厂（清配网 + 重启）

```json
{"cmd":"factory"}
```

- 参数：无
- 成功回包：`ack`（回包后延迟 ~300ms 再重启）
- 失败回包：无
- 特性：删除 `/wifi.cfg` 并重启，回到默认 AP 热点模式（`AIMonitor-<chipid>`，:80 配网门户可用）

**demo**
```python
mon_link.request({"cmd":"factory"})
# {"resp":"ack"}  之后设备重启回默认 AP
```

---

## 4. 设备 → PC 回包表

| 回包 | JSON | 含义 |
|---|---|---|
| ack | `{"resp":"ack"}` | 成功 |
| nack | `{"resp":"nack","code":n}` | 失败（见 §5） |
| ready | `{"resp":"ready","off":4096}` | 上传/流就绪，off 为续传起点 |
| done | `{"resp":"done"}` | 上传完成/流结束 |
| progress | `{"resp":"progress","seq":30,"fps":24}` | 播放进度（播放中周期上报，**广播**） |
| state | `{"resp":"state","state":"play","name":"x.anm","seq":10}` | 状态查询/变更结果（state 可为 idle/play/stream/home） |
| pong | `{"resp":"pong","fw":"1.3.2","proto":1,"ip":"192.168.4.1","fs_total":2981888,"fs_free":2800000}` | ping 响应 |
| wifi | `{"resp":"wifi","mode":"ap+sta","ssid":"MyWiFi","ip":"192.168.1.50","port":8088,"clients":1,"sta":1}` | wifi_status 响应 |
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
9. **TCP 通路（FW 1.2.0）**：协议 TCP Server 默认 `:8088`；`protocol.py` 提供 `TcpTransport`/`connect_tcp(host, port)`，`Link`/`AIMonitor` 无需改动。**FW 1.3.0 起回包路由**：命令应答只回请求来源，异步事件（progress/done）广播到串口 + 所有 TCP 客户端。
10. **单数据流限制**：串口与 TCP 共享同一协议状态机，`show/frame/up_chunk/transmit` 带数据体命令同一时刻只允许一个源（双源并发会串包）。Web `:80` 走独立通道（`transport_exec` 同步执行），不受影响。
11. **Web 控制面板（FW 1.3.0）**：门户在 AP/STA/ap+sta 任何模式均可达；`GET /status` 返回聚合状态快照（网络/存储/播放器/系统），`POST /cmd` 接受 JSON 命令（body 即命令对象），响应体为回包 JSON。
12. **安全**：TCP 无鉴权即可删/传文件，仅限局域网玩具用途。
