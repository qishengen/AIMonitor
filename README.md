# AIMonitor

用来展示 AI 状态的桌面小玩具：ESP-12F + SSD1306 128x64 OLED 动画播放器，配像素动画编辑器。支持**串口 + WiFi/TCP 双通路**控制，**浏览器 Web 配网**。

## 目录结构

```
Code/AIMonitor/            ESP8266 固件（FW 1.3.2：通信层/协议层分离 + WiFi/TCP + Web控制面板）
Code/AIMonitor/docs/       需求/设计/协议/开发摘要（DEV_CONTEXT.md 为后续开发入口）
Code/host/                 Python 上位机（protocol/aimonitor/anm_builder/pxa/ai_stream/tcp_test）
Code/host/tests/           回环自测：串口模拟 15/15 + TCP 模拟(TcpSimServer) 6/6
像素编辑器/                单文件像素动画编辑器 + PXA 分享格式（见 PXA格式.md）
scr/                       示例 GIF 素材（clawd-*.gif）
建模/                      3D 打印模型（壳体/后盖/横棍 .stl）
AIMonitor-32/              ESP32 旧原型，勿与 ESP8266 混淆
```

## 快速上手

1. **烧录**：`Code/AIMonitor` 用 Arduino IDE / arduino-cli 编译烧录（Flash Size 选 4M(FS:3M)，波特率 921600）。上电后**默认 AP+STA**（热点 `AIMonitor-xxxx` 常开），待机屏显示 WiFi 模式/名称/密码/TCP 端口。
2. **Web 控制面板**：手机/PC 连热点 `AIMonitor-xxxx`（**热点默认开放无密码**，可在配网表单「AP 热点密码」或 `ap_pass` 命令设置 ≥8 位密码）→ 浏览器开 `192.168.4.1` → 状态卡片（网络/存储/播放器/系统）+ 文件播放/删除 + 待机/主页/重启/恢复出厂/重连WiFi 按钮 + 配网表单（选 WiFi 填密码保存 → 切路由器，STA 失败 15s 自动退回热点）。门户 AP/STA 任何模式均可达。
3. **TCP 控制**：`python Code/host/tcp_test.py aimonitor.local`（或 `python tcp_test.py <设备IP>`）跑协议测试；也可连串口用原协议。
4. **编辑器**：浏览器打开 `像素编辑器/动画编辑器.html`（设备控制需 Chrome/Edge 且 localhost/https 打开，建议 `python -m http.server`）。画动画 → 上传/流式预览到设备。
5. **分享**：导出 `.pxa` 文本格式存 git，任何人可直接预览/导入。

## 协议

JSON 明文 + `\n`，帧数据体二进制或 base64 双模式。命令：ping/status/stop/list/delete/play/show/stream_*/up_*/transmit/wifi/wifi_status/**ap_pass/wifi_reconnect/home/reset/factory**。串口(921600) 与 TCP(:8088) 双通道，Web 控制面板(:80)。**命令应答只回请求来源，异步事件(进度/完成)广播**。详见 `Code/AIMonitor/docs/PROTOCOL.md` 与 `DEV_CONTEXT.md`。

## 要点速记（详见 DEV_CONTEXT.md）

- 帧数据 **LSB 位序**（字节 bit0=行最左），MSB 会左右镜像。
- GIF 编解码的 LZW 码宽时机很易错，改动需回归验证。
- 固件 IRAM 92%（WiFi 代码全进 Flash 没爆）、RAM 73%、Flash 31%，加代码需留意。
- 回包路由：命令应答回源，异步事件广播；带数据体命令同一时刻只允许一个源。

## TODO（原设想）

1. 构造协议，串口/蓝牙/WiFi 到单片机
2. 单片机做手脚：接屏显示、433 遥控，协议控制
3. 设计 skill 给 AI 装接口
