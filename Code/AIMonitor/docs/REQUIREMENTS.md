# AIMonitor 需求文档 (REQUIREMENTS)

> 版本: v1.0 ｜ 日期: 2026-08-07 ｜ 状态: 已评审定稿

## 1. 项目概述
- **项目名**: AIMonitor
- **定位**: ESP8266 (ESP-12F) 驱动的 SSD1306 128x64 OLED 动画播放器
- **目标**: 通过串口接收上位机(Python)指令，按指令播放 LittleFS 中保存的动画，或实时显示上位机下发的帧。上位机集成 AI，可将文字/图片/AI 生成的内容转成动画下发播放。

## 2. 硬件环境
| 项 | 规格 |
|---|---|
| 主控 | ESP-12F (ESP8266EX, 4MB Flash, RAM ~80KB) |
| 显示屏 | SSD1306 128x64, I2C (SCL=2, SDA=0) |
| 驱动库 | U8g2: `U8G2_SSD1306_128X64_NONAME_F_SW_I2C` (全屏缓冲 1024B) |
| 上位机 | Python 3 + pySerial |
| 串口波特率 | 921600 (可配) |

## 3. 功能需求

### FR1 内置动画播放
- 按动画名播放 LittleFS 中已保存的 `.anm` 动画
- 指令可指定 fps(0=用文件头存储值) 与是否循环
- 播放中周期性上报进度

### FR2 实时帧显示 (show)
- 上位机发送一帧 → 设备立即显示一帧
- **无帧率、无总帧数**，节奏完全由上位机控制
- 与任何模式无关，任意时刻可发（播放中/空闲/流中均生效）

### FR3 动画流播放 (stream)
- 上位机连续发送多帧，设备**按 fps 在设备端定时播放**，多帧组合成动画
- `total` 可选：给则收满自动结束；不给则用 `stream_end` 显式结束
- 帧早到排队(暂存1-2帧,超出丢最新帧策略)、晚到延时到点再播
- 不落盘，适合 AI 现场生成的临时动画

### FR4 动画写入保存 (upload / transmit)
- `upload`: 分块写入大动画，支持**断点续传**、逐块确认、整文件 CRC32 校验落盘
- `transmit`: **一次性整动画传入 → 仅保存 LittleFS**，不自动播放
- 保存后重启仍在（LittleFS 持久化），可用 `play` 随时播放
- 同名文件覆盖 = 更新动画

### FR5 动画库管理
- `list`: 列出 LittleFS 中所有 `.anm` 文件
- `delete`: 按名删除动画
- 管理对象均为**动画文件（整动画）**

### FR6 双向状态上报
- 每条有效指令回 `ack`，失败回 `nack` + 错误码
- 握手 `ping` 返回固件/协议版本、FS 容量
- `status` 返回当前播放状态/动画名/帧进度
- 播放进度上报 `progress`

### FR7 协议易扩展
- 协议头为 **JSON 明文文本**，`\n` 分隔，UTF-8
- 帧/块数据体支持双编码：**原始二进制**（默认，省体积）或 **base64 内嵌 JSON**（纯文本调试/传输）
- 未知字段忽略，向后兼容

### FR8 Python 上位机工具集
- `protocol.py`: JSON 编解码(bin/b64 双模式) + 串口封装
- `aimonitor.py`: 高层 API (play/show/stream/transmit/upload/list/delete/status/ping)
- `anm_builder.py`: 文字/图片/AI → .anm 文件
- `ai_stream.py`: AI 联动示例

## 4. 非功能需求
- **固件体积**: 目标 <= 250KB (尽量精简，动画数据不编译进固件)
- **RAM**: 总占用 < 6KB (U8g2 缓冲 1KB + 双缓冲 2KB + 串口环形缓冲 2KB + 行缓冲 256B + LittleFS 开销)
- **帧率**: OLED I2C 全屏刷新上限 ~30fps；串口 921600 下理论 ~90fps，非瓶颈
- **可靠性**: 坏包丢包重同步、上传断线续传、CRC 校验失败不落盘

## 5. 验收标准
1. PC 端回环模拟器跑通全部 11 条指令的收发与 CRC
2. 真机 `transmit` 上传 60 帧动画后 `play` 正常循环播放
3. `show` 逐帧直显即时响应
4. `stream` 24fps 下无明显卡顿，`total` 与 `stream_end` 两种结束方式均正常
5. 上传中途断电/断线，重连后 `up_chunk` 指定 offset 续传成功，CRC 校验通过
6. `list` / `delete` / `ping` / `status` 全部正常
