# 开发摘要上下文 (DEV_CONTEXT)

> 供明天(下一会话)直接继续开发，无需重新沟通。版本: v1.5, 2026-08-21

## 1. 一句话总结
ESP-12F + SSD1306 128x64 OLED 串口动画播放器 + **像素动画编辑器** + **PXA 文本分享格式** + **WiFi/TCP 双通路** + **Web 控制面板**。固件 FW 1.3.2 编译通过（IRAM 92% 无爆），Python 回环 15/15 + TCP 回环 6/6。**FW 1.3.0 将通信层(transport)与协议层(protocol)分离**：transport 统一收串口/TCP/Web 字节并组装帧，protocol 只做命令语义；回包**应答回源 + 事件广播**；Web 门户常开（AP/STA/ap+sta 均可达）并升级为控制面板（`/status` 状态快照 + `/cmd` 控制 + 文件播放/删除 + 待机/主页/重启/恢复出厂/重连WiFi 按钮）。**FW 1.3.1 默认 `ap+sta`（AP 常开）+ 热点密码与 STA 解耦；1.3.2 新增 `ap_pass` 命令（设置/清除 AP 热点密码，串口/TCP/Web 三通道统一）。** 剩真机验证（含 TCP/Web 配网/控制面板）。

## 2. 已确定的关键决策
- 主控: **ESP-12F** (ESP8266EX, 4MB Flash)
- 动画存储: **LittleFS 文件系统** (.anm 文件，动画数据不进固件)
- 双向状态上报: **需要**
- 网络（FW 1.2.0 新增）: **WiFi AP/STA/AP+STA** + 协议 **TCP Server :8088** + **Web 配网 :80**（手写 HTTP），mDNS `aimonitor.local`；纯 STA 连不上 15s 自动退 `ap+sta` 保门户；配置存 `/wifi.cfg`
- 待机屏（FW 1.2.0 新增）: IDLE 状态显示 **IP/TCP 端口/客户端数**（`draw_idle_info`），替代原内置方块动画
- 协议: **JSON 明文文本 + `\n` 分隔**，UTF-8，非二进制魔数头
- JSON 解析: **自写最小单层解析器** (不用 ArduinoJson，省 15-25KB Flash)
- **分层（FW 1.3.0 新增）**: 通信层 `transport.cpp`（串口+TCP+Web 字节收发/帧组装）与协议层 `protocol.cpp`（命令语义）分离；数据命令头由 transport 回调 `on_header` 判定
- **回包路由（FW 1.3.0 新增）**: 命令应答**只回请求来源**（串口/TCP 对应客户端/Web HTTP 响应）；异步事件（progress/done）**广播**串口+所有 TCP
- **Web 门户常开（FW 1.3.0 新增）**: `net_apply()` 后 web server 自动重绑（`webcfg_poll` 用 `net_generation()` 检测），AP/STA/ap+sta 任何模式均可达；默认配置仍为 `ap`
- 帧数据体: **双模式** — 默认原始二进制；`"enc":"b64"` 时 base64 内嵌 JSON
- transmit 行为: **仅保存，不自动播放**，用 play 手动播
- stream 播放节奏: **设备端按 fps 定时**
- show 命令: **完全独立**，任意时刻可发，无帧率/无总帧数，上位机控节奏

## 3. 完整命令清单 (PC -> ESP)
```
{"cmd":"ping"}
{"cmd":"status"}
{"cmd":"stop"}
{"cmd":"list"}
{"cmd":"delete","name":"x.anm"}
{"cmd":"play","name":"x.anm","fps":24,"loop":1}
{"cmd":"show","seq":5,"size":1024}               + 1024B / 或 enc:b64
{"cmd":"stream_start","fps":24,"loop":1}          # total 可选
{"cmd":"frame","seq":0,"size":1024}               + 1024B / 或 enc:b64
{"cmd":"stream_end"}
{"cmd":"up_begin","name":"demo.anm","size":61448}
{"cmd":"up_chunk","off":0,"size":4096}            + 4096B / 或 enc:b64
{"cmd":"up_end","crc":1234567890}
{"cmd":"transmit","name":"demo.anm","fps":24,"loop":1,"size":61440} + 整包
{"cmd":"wifi","mode":"ap|sta|ap+sta","ssid":"..","pass":"..","port":8088}   # 可选字段，持久化+延迟~100ms应用
{"cmd":"ap_pass","pass":"..|''"}        # 设置/清除 AP 热点密码（≥8 位 WPA2，空=开放；FW 1.3.2）
{"cmd":"wifi_status"}
{"cmd":"wifi_reconnect"}        # 用已存配置重连（FW 1.3.0）
{"cmd":"home"}                  # 播放内置主页动画 idle_anim（FW 1.3.0）
{"cmd":"reset"}                 # 重启设备，ack 后延迟 300ms 重启（FW 1.3.0）
{"cmd":"factory"}               # 删 /wifi.cfg + 重启回默认 AP（FW 1.3.0）
```

## 4. 回包清单 (ESP -> PC)
```
{"resp":"ack"}   {"resp":"nack","code":1}   {"resp":"ready"}   {"resp":"done"}
{"resp":"progress","seq":30,"fps":24}
{"resp":"state","state":"play","name":"x.anm","seq":10}   # state: idle|play|stream|home
{"resp":"pong","fw":"1.3.0","proto":1,"ip":"192.168.4.1","fs_total":...,"fs_free":...}
{"resp":"wifi","mode":"ap+sta","ssid":"MyWiFi","ip":"192.168.1.50","port":8088,"clients":1,"sta":1}
{"resp":"list","anims":["a.anm","b.anm"]}
```
错误码: 1=未知命令 2=参数错 3=文件不存在 4=CRC错 5=存储满
> 路由（FW 1.3.0）：命令应答只回请求来源；progress/done 等异步事件广播到串口+所有 TCP。

## 5. .anm 文件格式
```
0-3 "ANM0" | 4 fps | 5-6 frame_count(LE) | 7 保留 | 8.. 每帧1024B裸XBM
```

## 6. 目标文件结构 (已完成)
```
AIMonitor.ino      主循环: transport_poll + net_poll + webcfg_poll + player_tick + protocol_tick；待机信息屏 draw_idle_info
version.h          FW_VERSION/PROTO_VERSION/SERIAL_BAUD 统一常量
transport.h/cpp    通信层(新)：串口+TCP+Web 源抽象 + 帧组装状态机(RX_LINE/RX_B64/RX_TAIL/RX_DATA) + 回源/广播 + transport_exec 供 Web 同步执行
json.h/cpp         最小JSON解析 + base64流式解码
protocol.h/cpp     协议层：命令分发 + 数据 sink(player/fs) + 新命令(reset/factory/home/wifi_reconnect) + 应答回源/事件广播；延迟重启/延迟重连(protocol_tick)
status.h/cpp       状态快照(新)：聚合 net/fs/player/system → Web /status 用
player.h/cpp       状态机(IDLE/PLAY/STREAM/HOME) + 双缓冲2KB + millis fps定时 + 待机信息屏回调(idle_info_cb) + home 内置动画
fs.h/cpp           LittleFS .anm 读写/续传/CRC32/list_array
net.h/cpp          WiFi(AP/STA/AP+STA) + TCP Server(:8088) + /wifi.cfg 配置(含 ap_ssid) + mDNS + STA失败15s退ap+sta兜底 + net_client_read/write + net_generation(web重绑)
webcfg.h/cpp       Web 门户(:80) 常开：GET / /scan /cfg /status，POST /save /cmd；控制面板页(状态卡片+文件播放/删除+功能按钮+配网表单)
--- 上位机 (G:\AIMonitor\Code\host) ---
protocol.py        JSON编解码(bin/b64) + 传输抽象(SerialTransport/TcpTransport/Link) + connect_tcp(host,port)
aimonitor.py       高层API play/show/stream/transmit/upload/list/delete/status/ping
anm_builder.py     文字(Pillow)/图片/GIF/AI -> .anm（to_xbm 为 LSB）
pxa.py             PXA 文本格式编解码 (to_pxa/parse_pxa)，与 .anm/XBM 互转
ai_stream.py       AI联动示例(占位生成器已改 LSB)
tcp_test.py        TCP 通路测试 (python tcp_test.py [host] [port] [--sim] [--upload x.anm])，--sim 回环 6/6
--- 测试 (G:\AIMonitor\Code\host\tests) ---
sim_firmware.py    固件行为模拟器 + TcpSimServer(挂本地 TCP 端口模拟固件 server) + wifi/wifi_status/home/reset/factory/wifi_reconnect 命令模拟
test_loopback.py   PC回环自测 15/15 通过 (python test_loopback.py)
--- 像素编辑器 (G:\AIMonitor\像素编辑器) ---
动画编辑器.html    单文件编辑器：黑白像素画/动画 + 设备控制 + PXA + GIF + 导入预览
动画编辑器需求.md  编辑器需求与版本历史
PXA格式.md         PXA 分享格式规格
```
注: 旧的 IMG.h / wait.h / Base.h (ESP32原型) 已删除/移至 AIMonitor-32，不复用。

## 7. 关键参数速查
- 单帧 128x64/8 = 1024B；1秒@24fps = 24KB；LittleFS 可用 ~2.5MB
- 串口 921600；**协议 TCP :8088；Web 配网 :80**；mDNS `aimonitor.local`；显示上限 ~30fps (I2C瓶颈)
- RAM 预算 <6KB；固件目标 <=250KB（已超出到 ~320KB，可后续裁剪）
- **位序（重要）**：设备显示 `u8g2.drawXBMP` 为 **LSB 位序**（字节 bit0=行最左像素）。所有帧数据生成必须用 LSB：
  - 编辑器 `frameToDeviceBytes`（上传/show/流式）
  - `anm_builder.py to_xbm`
  - 内置 `idle_anim.h`（数组生成脚本：`out[y*8+x//8] |= 1 << (x%8)`）
  - 用 MSB 会导致图像左右镜像。

## 8. 实施顺序 (进度)
1. 协议层: json.h/cpp + protocol.h/cpp + PC 回环自测 —— ✅ 完成
2. 播放器: 状态机 + play —— ✅ 完成
3. 实时: show + stream 双缓冲 —— ✅ 完成
4. 写入: upload + transmit + list/delete —— ✅ 完成
5. Python 工具集 —— ✅ 完成
6. WiFi/TCP 通路: net.h/cpp + protocol_feed/广播 + wifi 命令 + TcpTransport/tcp_test —— ✅ 完成
7. Web 配网: webcfg.h/cpp + STA 兜底 + mDNS + 待机 IP 屏 —— ✅ 完成（编译通过，剩真机验证）
8. 真机验证 —— ⏳ 待做（需要硬件在环）

## 9. 已修复的 bug（v1.1 会话）
- **transmit 双头**：上位机 payload 已含 8B .anm 头，固件 `fs_transmit_begin` 又写一次头 → 文件=16B+帧、无法播放。改为 payload 原样落盘（`fs_transmit_begin(name)` 不再写头）。loopback 增加 `parse_anm` 校验回归。
- **fs.cpp g_name 共享冲突**：upload/transmit 与 play 共用同一静态文件名，播放中途上传会串文件。拆分为 `g_up_name` / `g_anim_name`。
- **player.cpp `loop` 变量名**：与 Arduino 保留 `loop()` 冲突，编译报错。改名 `looping`。
- **串口高速丢字节（FW 1.1.0）**：渲染上屏(I2C ~26ms)或写 Flash 阻塞期间，内核 Serial 接收缓冲仅 256B+FIFO128B，1KB+ 帧数据突发会溢出丢字节 → 设备卡在 RX_DATA 永久不回包。修复：`setup()` 里 `Serial.setRxBufferSize(4096)`（必须在 begin 前）；协议环形缓冲 2048→8192；批写 64→256；新增 1s 收包超时（数据不完整即复位回 nack(2)，不再卡死）。
- **编辑器上传/流式**：上传分块 4096→2048、加 ping 握手、失败自动重试一次、nack 直接报真实错误码；流式预览先 `stop` 复位、`frame` 并发保护、progress 日志节流。
- **GIF 导出坏文件（v1.2 会话）**：图像描述符漏本地颜色表标志(写 0x00 又加多余字节)、min code size 硬编码 8、LZW 首个新码从 clear 开始、码宽增长时机错一个、位打包用有符号 `>>` 溢出。已修，PIL 实测 8×8~200×200 精确解码。
- **GIF 导入残缺（v1.2 会话）**：缺逐帧合成，部分帧/透明/处置方式动画只显示局部矩形。已加 `compositeAll()`（处置 0/1/2/3）+ LZW end 码占位，clawd 真机 gif 像素级一致。

## 10. 编译验证（已通过）
```
arduino-cli compile --fqbn esp8266:esp8266:generic:xtal=160,vt=flash,exception=disabled,\
stacksmash=disabled,ssl=all,mmu=3232,non32xfer=fast,FlashMode=dout,FlashFreq=40,eesz=4M3M,\
led=2,ip=lm2f,dbg=Disabled,lvl=None____,wipe=none,baud=921600 "G:\AIMonitor\Code\AIMonitor"
```
- 结果: EXITCODE=0（FW 1.3.2）。RAM 59276/80192(73%)；IRAM 60851/65536(92%)；Flash 330308B(31%)。
- 依赖: esp8266 core 3.1.2 + U8g2 2.36.19（已装）。
- 注意: WiFi 栈代码几乎全进 Flash（Flash +~49KB），IRAM 仅 +436B，没爆 92% 上限；RAM +~15KB（lwip/HTTP 缓冲/扫描），heap 缩水，加代码留意。
- **编译排坑**：本次曾两次 gcc 因临时文件失败（ICE segfault / `error writing to ...\.s`，磁盘充足，疑杀软）。设 `$env:TMP/TEMP` 到干净目录（如 `...\Temp\opencode\cc_tmp`）后正常。

## 11. 开启动画（内置程序化）
- 在 `AIMonitor.ino` `setup()` 中 `u8g2.begin()` 后阻塞播放约 1.5s：收缩状态圈 + "AIMonitor" 品牌字 + 底部进度条（带奔跑方块），结束后清屏进入主循环。
- 纯 u8g2 图元绘制，无文件/无帧缓冲依赖；用 `u8g2_font_6x13_tf` 字体（+~1KB Flash）。
- 期间串口仍能缓存少量命令（如 ping），主循环开始后正常处理；改动该动画只需改 `boot_animation()`。

## 12. 待机屏（IDLE 状态）
- **FW 1.2.0 起：IDLE 显示配网信息屏**（替代方块动画）。`player.cpp` 注册 `idle_info_cb`，`AIMonitor.ino` 的 `draw_idle_info()` 用 u8g2 6x13 画 4 行：标题 `AIMonitor` + `模式 名称`（`ap+sta AIMonitor-xxxx`，AP 开则热点名否则 STA ssid，`net_mode_str`/`net_ap_ssid_str`/`net_ssid_str`）+ `PASS xxx`（空=开放热点，`net_pass_str`）+ `TCP 8088 C:客户端数`。右上角呼吸点每 700ms 闪。**不显示 Web 端口。**
- **FW 1.3.1 起默认模式 `ap+sta`（AP 常开）**：`net_default_cfg` 默认 `ap+sta`，热点恒可用，STA 有 ssid 才连；**AP 热点密码独立**（1.3.2 `ap_pass` 命令可设，空=开放），待机屏 `PASS` 行显示 `ap_pass` 或 `PASS (open)`。
- 方块动画兜底：未注册 idle_info_cb 时仍用 `idle_anim.h`（64x64 帧数组，`idle_render()` 居中合成）。改动待机屏只改 `draw_idle_info()`。
- **FW 1.3.0：新增 `home` 命令播内置主页动画**（`player_play_home`，状态 ST_HOME，循环播放 idle_anim 直至 stop/show/play/stream；`show` 在 home 下先回 idle-hold）。
- 交互不变：`show` 置 `idle_hold=true` 暂停（保持所显帧），`play`/`stream` 结束或 `stop` 时 `idle_resume()` 恢复信息屏。

## 13. WiFi / TCP / Web 控制面板（FW 1.2.0 新增，1.3.0 升级）
- **net.h/cpp**：配置结构 `net_cfg_t`（mode/ssid/pass/ap_ssid/**ap_pass**/port/静态IP），存 `/wifi.cfg`（key=value 文本）。`net_setup()` 读取并 `net_apply()`：`WiFi.mode` 按 `ap|sta|ap+sta`、**AP 名用 `ap_ssid` 否则 `AIMonitor-<chipid>`（1.3.0 修复：不再误用 STA ssid 当热点名）**、STA 支持 DHCP/静态 IP；`WiFiServer(:8088)` + `net_client_write(idx,...)` 单发/`net_broadcast()` 广播（上限 2）。**默认模式 `ap+sta`（1.3.1，AP 常开）**。**AP 热点密码独立（1.3.2 新增 `ap_pass` 命令）**：1.3.1 曾固定开放（`pass` 曾复用为热点密码导致手机"验证身份失败"，已解耦）；现空=开放、≥8 位=WPA2（`WiFi.softAP(ssid, ap_pass? : NULL, 1, 0, 8)`，客户端上限 8）；`net_ap_pass_str()` 供待机屏/状态展示。**改网命令（wifi/ap_pass/wifi_reconnect）统一"先 ack 后延迟 ~100ms 应用"（`g_reapply_at`）**，保证串口/TCP/Web 各通道先收到回包。
- **通信层/协议层分离（FW 1.3.0）**：`transport.cpp` 统一收串口/TCP/Web 字节（同一 8KB ring）并组装帧，回调 `on_line/on_header/on_payload/on_done/on_timeout` 交给 `protocol.cpp`；`transport_exec()` 供 Web 同步执行命令并捕获回包。**回包路由：命令应答回源（`transport_send_line`）、异步事件广播（`transport_broadcast_line`）**。`show/frame/up_chunk/transmit` 同一时刻只允许一个源（单管线，天然限制）。
- **命令（1.3.0 新增）**：`home`（内置主页动画）、`reset`（ack 后延迟 300ms 重启）、`factory`（删 /wifi.cfg + 重启回默认 AP）、`wifi_reconnect`（重读配置延迟 100ms 重连，`protocol_tick` 执行）。`ping` 的 pong 含 `ip`。协议版本 `proto=1`，FW `1.3.0`。
- **webcfg.h/cpp**：手写 HTTP 门户 `:80`（不引 ESP8266WebServer，保 IRAM）。路由 `GET /`(控制面板) `/status`(状态快照) `/scan`(AP列表) `/cfg`(预填) `POST /save` `/cmd`。**`/cmd` 用 `protocol_web_exec`（1.3.1 修复：不经过 transport 帧状态机，独立解析分发，串口/TCP 数据包占用时不再返回 busy/失败）；`WEB_MAX_CLIENTS` 2→4（首页 4 个并行 fetch，防丢连接）**。**门户常开**：`webcfg_poll` 用 `net_generation()` 检测 `net_apply` 后重绑 server，AP/STA/ap+sta 均可达。配置页 JS 用 esprima 校验过；HTTP 解析逻辑有 Python 镜像测试（header_end/content_length/url_decode/分块/中文 SSID）。
- **STA 兜底**：纯 `sta` 15s 连不上 → 运行时自动退 `ap+sta` 保门户（不持久化）；mDNS `aimonitor.local`（STA 联网后可 `http://aimonitor.local`）。
- **待机屏**：IDLE 显示 IP/TCP 端口/客户端数（见 §12）。
- **上位机**：`protocol.py` 加 `TcpTransport`/`connect_tcp(host,port)`，`Link`/`AIMonitor` 零改动；`tcp_test.py` 真机/回环测试（`--sim` 用 `sim_firmware.TcpSimServer`，6/6）。编辑器仍走 Web Serial，未接 TCP/WebSocket（后续可选）。

## 14. 像素编辑器（G:\AIMonitor\像素编辑器\动画编辑器.html）
- 单文件纯前端，无依赖。**纯黑白**（彩色已移除），画布尺寸 16/32/64/64×32/128×64(设备)。
- 设备面板用 **Web Serial API**（Chrome/Edge，需 localhost/https 打开；file:// 下不可用）：连接→上传动画(分块+CRC)、发送当前帧、设备播放/停止、流式预览、状态/删除。
- 帧→设备 128×64 XBM：`frameToDeviceBytes`，**等比缩放居中**（任意画布尺寸），LSB 位序。
- 选择/移动：左键拖拽=框选(抬起结束)，选中区内拖拽=像素实时跟随，右键取消选择。
- 关键校验命令：
  - JS 语法/ID：`python C:\Users\90346\AppData\Local\Temp\opencode\check_js.py`（esprima）
  - 帧/CRC/.anm/PXA 逻辑：`check_logic.py` / `check_chain.py` / `check_pxa.py`
  - GIF 编码 vs PIL：`check_final.py`；解码 vs 真实 gif：`check_content.py`
- 注意：仓库里 `动画编辑器 - 副本.html` 是旧备份，别改错文件。

## 15. PXA 分享格式（git 友好）
- 纯文本，`;` 元数据行 + `#`=黑 `.`=白 像素行；`;PXA v1` 魔数、`;W= H= F= FPS= LOOP=`、`;NAME=`、`;---- FRAME N ----` 分隔。
- 任意尺寸，编辑器可导入/导出；Python 端 `host/pxa.py`；与 .anm 互转仅限 128×64。
- 规格见 `像素编辑器\PXA格式.md`。

## 16. GIF 编解码要点（容易再踩坑，改动需回归验证）
- **码宽增长时机**（最易错）：编码器用 `nextCode >= (1<<codeSize)+1`（比字典计数晚一个），与标准解码器"首个新码由第 2 码经 KwKwK 构建"对齐。改回 `>= (1<<codeSize)` 会导出坏文件。
- LZW 首个新码 = `clear+2`；位打包必须用无符号 `>>>`（有符号 `>>` 溢出）。
- 调色板补齐 2 的幂；min code size 与 LZW 一致；图像描述符 packed 含本地颜色表标志 `0x80|(bits-1)`。
- 解码端 `compositeAll()` 逐帧合成（部分帧/透明/处置 0/1/2/3），先合成再缩放。
- 回归：`check_final.py`（编码 vs PIL）、`check_content.py`（解码 vs 真实 gif）。

## 17. 导入图片设计（静态图片）
- 交互式「导入预览」：等比缩放居中 → 拖动移动 + 滚轮缩放(光标为中心) + ↔ 适应复位 → 应用写当前帧/取消。
- 二值化三种模式：
  - 填充(全局阈值)：`gray < T` 黑。
  - **自适应(局部对比)**：`gray < 局部均值 - C`（邻域约 1/6 画布），按局部明暗保留内部细节（照片/渐变适用）。
  - 轮廓(边缘 Sobel)：梯度幅值 > T 黑，出线稿。
- 空白区按白处理；GIF 导入走等比适应 + 阈值。

## 18. 下一步(推荐起点)
1. 真机验证（优先级）：
   - 串口/TCP 双通路：`tcp_test.py aimonitor.local`（或 AP 下 `tcp_test.py 192.168.4.1`）跑 ping/wifi_status/transmit/play/show/stream。
   - **Web 控制面板（1.3.0 新）**：连热点 `AIMonitor-xxxx` → `192.168.4.1` → 状态卡片(网络/存储/播放器/系统)自动刷新、文件列表播放/删除、按钮(待机/主页动画/重连WiFi/重启/恢复出厂)；配网表单保存后设备切 STA → `aimonitor.local` 或路由 IP 再验证；纯 sta 断网 15s 验证退 ap+sta。
   - 原有项：`transmit` 60帧 → `play` 循环；`show` 即时；`stream` 24fps 不卡；断线续传 + CRC。
2. 真机串口 921600，接线 SCL=5/SDA=4（当前 .ino 已配 ESP8266；AIMonitor-32 是旧 ESP32 原型，别混淆）。
3. 上传 60 帧素材：编辑器画 → 上传；或 `scr/` gif：`anm_builder.frames_from_gif` → `mon.transmit`。
4. 固件资源：IRAM 92%（WiFi 全进 Flash 没爆，再加代码小心）；RAM 73%（transport/status/webcfg/Web槽×4 新增）；Flash 31%。
