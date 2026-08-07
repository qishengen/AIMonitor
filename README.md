# AIMonitor

用来展示 AI 状态的桌面小玩具：ESP-12F + SSD1306 128x64 OLED 串口动画播放器，配像素动画编辑器。

## 目录结构

```
Code/AIMonitor/            ESP8266 固件（FW 1.1.0）
Code/AIMonitor/docs/       需求/设计/开发摘要（DEV_CONTEXT.md 为后续开发入口）
Code/host/                 Python 上位机（protocol/aimonitor/anm_builder/pxa/ai_stream）
Code/host/tests/           PC 回环自测（13/13）
像素编辑器/                单文件像素动画编辑器 + PXA 分享格式（见 PXA格式.md）
scr/                       示例 GIF 素材（clawd-*.gif）
AIMonitor-32/              ESP32 旧原型，勿与 ESP8266 混淆
```

## 快速上手

1. **固件**：`Code/AIMonitor` 用 Arduino IDE / arduino-cli 编译烧录（Flash Size 选 4M(FS:3M)，波特率 921600）。
2. **编辑器**：浏览器打开 `像素编辑器/动画编辑器.html`（设备控制需 Chrome/Edge 且 localhost/https 打开，建议 `python -m http.server`）。画动画 → 上传/流式预览到设备。
3. **分享**：导出 `.pxa` 文本格式存 git，任何人可直接预览/导入。

## 协议

JSON 明文 + `\n`，帧数据体二进制或 base64 双模式。命令：ping/status/stop/list/delete/play/show/stream_*/up_*/transmit。详见 `Code/AIMonitor/docs/DESIGN.md` 与 `DEV_CONTEXT.md`。

## 要点速记（详见 DEV_CONTEXT.md）

- 帧数据 **LSB 位序**（字节 bit0=行最左），MSB 会左右镜像。
- GIF 编解码的 LZW 码宽时机很易错，改动需回归验证。
- 固件 IRAM 92%、Flash 272KB，加代码需留意。

## TODO（原设想）

1. 构造协议，串口/蓝牙/WiFi 到单片机
2. 单片机做手脚：接屏显示、433 遥控，协议控制
3. 设计 skill 给 AI 装接口
