# AIMonitor 方案设计文档 (DESIGN)

> 版本: v1.0 ｜ 日期: 2026-08-07 ｜ 状态: 已评审定稿
> **v1.2 扩展（FW 1.2.0）**：架构上新增 `net`（WiFi + TCP Server :8088 + `/wifi.cfg`）与 `webcfg`（Web 配网 :80）两模块；协议状态机由 `protocol_feed()` 统一喂入串口/TCP 字节，`send()` 广播到串口 + 所有 TCP 客户端。详见 `DEV_CONTEXT.md` §13。
> **v1.3 扩展（FW 1.3.0）**：通信层/协议层分离 —— `transport`（串口+TCP+Web 统一字节收发/帧组装）与 `protocol`（命令语义）解耦；回包路由改为**命令应答回源 + 异步事件广播**；Web 门户常开并升级为控制面板（`/status`/`/cmd`）；新增 `home`/`reset`/`factory`/`wifi_reconnect` 命令。权威参考以 `PROTOCOL.md` 与 `DEV_CONTEXT.md` 为准。

## 1. 可行性评估
| 环节 | 约束 | 结论 |
|---|---|---|
| 显示刷新 | SSD1306 经 I2C 全屏刷一帧 ~26-35ms(400kHz) | 上限 ~30fps，动画足够 |
| 串口带宽 | 921600 baud ~90KB/s，单帧 1KB | 理论 ~90fps，非瓶颈 |
| 内存 | ESP-12F 堆 ~40-50KB；方案总占用 <6KB | 充裕 |
| Flash | 4MB (固件 1MB + LittleFS 3MB) | 存储量大 |

## 2. 存储容量计算
- 单帧 128x64/8 = **1024B = 1KB**
- 1 秒动画 @24fps = 24KB；@30fps = 30KB

| 项 | 计算 | 结果 |
|---|---|---|
| LittleFS 可用 | 3MB 分区扣开销 | ~2.5MB |
| 可存全屏帧 | 2.5MB / 1KB | ~2500 帧 |
| 一个动画(60帧@24fps≈2.5s) | 60KB + 8B 头 | 可存 ~40 个 |
| 连续播放上限 | 2500帧@24fps | ~100 秒 |
| 固件体积 | 动画不编译进固件 + 精简代码 | ~150-250KB |

## 3. 动画文件格式 `.anm` (LittleFS)
```
偏移   字段
0-3    "ANM0" 魔数(4B)
4      fps (1B)
5-6    frame_count 小端 (2B)
7      保留 (1B)
8..    每帧 1024B 裸 XBM，顺序拼接
```

## 4. 串口协议

### 通用约定
- 每包 = 一行 JSON 文本 (UTF-8)，以 `\n` 结尾
- 带数据体的包：JSON 头换行后紧跟 `size` 字段指定长度的原始字节
- 数据体双编码：默认二进制；`"enc":"b64"` 时数据为 JSON 内嵌 base64 字符串 `data`
- 设备对每条有效命令回 `ack`，失败回 `nack` + 错误码

### 4.1 PC -> ESP8266 命令
| 分类 | 命令 | JSON 示例 | 说明 |
|---|---|---|---|
| 控制 | ping | `{"cmd":"ping"}` | 握手，回版本/FS容量 |
| 控制 | status | `{"cmd":"status"}` | 回当前状态 |
| 控制 | stop | `{"cmd":"stop"}` | 停止播放 |
| 管理 | list | `{"cmd":"list"}` | 列动画名 |
| 管理 | delete | `{"cmd":"delete","name":"x.anm"}` | 删除动画 |
| 管理 | play | `{"cmd":"play","name":"x.anm","fps":24,"loop":1}` | 播放动画 |
| 实时 | show | `{"cmd":"show","seq":5,"size":1024}` + 1024B | 立即直显一帧，无帧率/总数，完全独立 |
| 实时 | stream_start | `{"cmd":"stream_start","fps":24,"loop":1}` | 动画流开始(total 可选) |
| 实时 | frame | `{"cmd":"frame","seq":0,"size":1024}` + 1024B | 流中一帧，设备按 fps 定时播放 |
| 实时 | stream_end | `{"cmd":"stream_end"}` | 无 total 时显式结束 |
| 写入 | up_begin | `{"cmd":"up_begin","name":"demo.anm","size":61448}` | 开始写入，回 ready |
| 写入 | up_chunk | `{"cmd":"up_chunk","off":0,"size":4096}` + 4096B | 分块写，off 支持断点续传 |
| 写入 | up_end | `{"cmd":"up_end","crc":1234567890}` | CRC32 校验落盘 |
| 写入 | transmit | `{"cmd":"transmit","name":"demo.anm","fps":24,"loop":1,"size":61440}` + 整包 | 整动画一次传入，仅保存 |

所有带数据体命令均支持 `"enc":"b64","data":"..."` 变体。

### 4.2 ESP8266 -> PC 回包
| 回包 | JSON | 含义 |
|---|---|---|
| ack | `{"resp":"ack"}` | 成功 |
| nack | `{"resp":"nack","code":n}` | 失败 (1=未知命令 2=参数错 3=文件不存在 4=CRC错 5=存储满) |
| ready | `{"resp":"ready"}` | 上传/流就绪 |
| done | `{"resp":"done"}` | 上传完成/流结束 |
| progress | `{"resp":"progress","seq":30,"fps":24}` | 播放进度 |
| state | `{"resp":"state","state":"play","name":"x.anm","seq":10}` | 状态查询/变更 |
| pong | `{"resp":"pong","fw":"1.0.0","proto":1,"fs_total":2981888,"fs_free":2800000}` | ping 响应 |
| list | `{"resp":"list","anims":["a.anm","b.anm"]}` | list 响应 |

## 5. 系统架构

### 5.1 ESP8266 固件
```
AIMonitor.ino    主循环: 非阻塞轮询串口 + 驱动播放器
json.h/cpp       最小单层 JSON 解析器 + base64 流式解码(4字符→3字节直写缓冲)
protocol.h/cpp   行缓冲(256B) + 命令分发表(CMD串→handler 函数指针)
player.h/cpp     播放状态机(IDLE/PLAY/SHOW/STREAM) + 双缓冲 + millis fps 定时
fs.h/cpp         LittleFS 读写 .anm / 断点续传 / CRC32
```

**内存预算**: U8g2 缓冲 1KB + 流式双缓冲 2KB + 串口环形缓冲 2KB + 行缓冲 256B + LittleFS 开销 ≈ **<6KB**

**播放器状态机**: IDLE / PLAY_ANIM(读FS) / STREAM(fps定时) / SHOW(即时) 四态

### 5.2 Python 上位机
```
protocol.py     JSON 编解码(bin/b64 双模式) + pySerial 封装 + 回包解析
aimonitor.py    高层 API: play/show/stream/transmit/upload/list/delete/status/ping
anm_builder.py  文字(Pillow)/图片/GIF/AI → .anm
ai_stream.py    AI 联动示例: 生成关键帧 → 流式播放
```

## 6. 优化与拓展要点
- 动画数据全部走 LittleFS，固件小巧
- 命令表为 CMD字符串→handler 映射，加指令只追加一行
- JSON 未知字段忽略 → 向后兼容
- base64 流式解码不整行缓存，省 RAM
- 流播放 fps 用 millis() 节流；LittleFS 逐帧顺序读(1KB <1ms)

## 7. 实施步骤
1. **协议层**: json.h/cpp + protocol.h/cpp + CRC，PC 端回环模拟自测
2. **播放器**: 状态机 + play(LittleFS 读 .anm 播放)
3. **实时显示**: show + stream_start/frame/stream_end，双缓冲边收边播
4. **写入保存**: up_begin/chunk/end + transmit + list/delete
5. **Python 工具集**: protocol → aimonitor → anm_builder → ai_stream
6. **实测**: 帧率/丢帧率/断线续传/稳定性

## 8. 验证方案
- PC 回环模拟器(虚拟串口或本地管道)跑通全指令
- 真机: transmit 60帧 → play 循环；show 即时；stream 24fps
- 断线续传: 中途中断后 up_chunk 指定 off 续传，CRC 通过
- Serial Monitor 人工校验 JSON 明文可读性
