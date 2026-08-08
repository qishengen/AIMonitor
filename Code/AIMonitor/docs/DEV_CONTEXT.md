# 开发摘要上下文 (DEV_CONTEXT)

> 供明天(下一会话)直接继续开发，无需重新沟通。版本: v1.2, 2026-08-07

## 1. 一句话总结
ESP-12F + SSD1306 128x64 OLED 串口动画播放器 + **像素动画编辑器** + **PXA 文本分享格式**。固件 FW 1.1.0 编译通过，Python 回环测试 13/13，编辑器含设备直连(Web Serial 上传/流式预览)、PXA 导入导出、GIF 编解码、导入预览(等比/自适应/轮廓)。**剩真机验证。**

## 2. 已确定的关键决策
- 主控: **ESP-12F** (ESP8266EX, 4MB Flash)
- 动画存储: **LittleFS 文件系统** (.anm 文件，动画数据不进固件)
- 双向状态上报: **需要**
- 协议: **JSON 明文文本 + `\n` 分隔**，UTF-8，非二进制魔数头
- JSON 解析: **自写最小单层解析器** (不用 ArduinoJson，省 15-25KB Flash)
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
```

## 4. 回包清单 (ESP -> PC)
```
{"resp":"ack"}   {"resp":"nack","code":1}   {"resp":"ready"}   {"resp":"done"}
{"resp":"progress","seq":30,"fps":24}
{"resp":"state","state":"play","name":"x.anm","seq":10}
{"resp":"pong","fw":"1.1.0","proto":1,"fs_total":...,"fs_free":...}
{"resp":"list","anims":["a.anm","b.anm"]}
```
错误码: 1=未知命令 2=参数错 3=文件不存在 4=CRC错 5=存储满

## 5. .anm 文件格式
```
0-3 "ANM0" | 4 fps | 5-6 frame_count(LE) | 7 保留 | 8.. 每帧1024B裸XBM
```

## 6. 目标文件结构 (已完成)
```
AIMonitor.ino      主循环: protocol_poll + player_tick
json.h/cpp         最小JSON解析 + base64流式解码
protocol.h/cpp     行缓冲256B + 命令分发表 + 协议状态机(RX_LINE/RX_B64/RX_TAIL/RX_DATA)
player.h/cpp       状态机(IDLE/PLAY/STREAM) + 双缓冲2KB + millis fps定时
fs.h/cpp           LittleFS .anm 读写/续传/CRC32
--- 上位机 (G:\AIMonitor\Code\host) ---
protocol.py        JSON编解码(bin/b64) + 传输抽象(SerialTransport/Link)
aimonitor.py       高层API play/show/stream/transmit/upload/list/delete/status/ping
anm_builder.py     文字(Pillow)/图片/GIF/AI -> .anm（to_xbm 为 LSB）
pxa.py             PXA 文本格式编解码 (to_pxa/parse_pxa)，与 .anm/XBM 互转
ai_stream.py       AI联动示例(占位生成器已改 LSB)
--- 测试 (G:\AIMonitor\Code\host\tests) ---
sim_firmware.py    固件行为模拟器
test_loopback.py   PC回环自测 13/13 通过 (python test_loopback.py)
--- 像素编辑器 (G:\AIMonitor\像素编辑器) ---
动画编辑器.html    单文件编辑器：黑白像素画/动画 + 设备控制 + PXA + GIF + 导入预览
动画编辑器需求.md  编辑器需求与版本历史
PXA格式.md         PXA 分享格式规格
```
注: 旧的 IMG.h / wait.h / Base.h (ESP32原型) 已删除/移至 AIMonitor-32，不复用。

## 7. 关键参数速查
- 单帧 128x64/8 = 1024B；1秒@24fps = 24KB；LittleFS 可用 ~2.5MB
- 串口 921600；显示上限 ~30fps (I2C瓶颈)
- RAM 预算 <6KB；固件目标 <=250KB
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
6. 真机验证 —— ⏳ 待做（需要硬件在环）

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
- 结果: EXITCODE=0。RAM 36984/80192(46%，应用静态缓冲约 4.5KB 符合 <6KB 预算)；Flash 272KB(26%)。
- 依赖: esp8266 core 3.1.2 + U8g2 2.36.19（已装）。
- 注意: IRAM 92%(60415/65536) 偏高，后续加代码可能链接失败；Flash 略超 250KB 目标，可后续用 `-Os`/裁剪 U8g2 字体优化（不阻塞）。

## 11. 开启动画（内置程序化）
- 在 `AIMonitor.ino` `setup()` 中 `u8g2.begin()` 后阻塞播放约 1.5s：收缩状态圈 + "AIMonitor" 品牌字 + 底部进度条（带奔跑方块），结束后清屏进入主循环。
- 纯 u8g2 图元绘制，无文件/无帧缓冲依赖；用 `u8g2_font_6x13_tf` 字体（+~1KB Flash）。
- 期间串口仍能缓存少量命令（如 ping），主循环开始后正常处理；改动该动画只需改 `boot_animation()`。

## 12. 待机动画（内置数组，IDLE 状态循环）
- 新增 `idle_anim.h`：64x64 帧数组（`IDLE_ANIM_W/H/FRAME_COUNT`、`idle_anim`），const 存 Flash（~1KB），示例为 2 帧"外框→实心"呼吸方块，可按需替换数组。
- `player.cpp` IDLE 状态下按 700ms 循环合成上屏：`idle_render()` 把 64x64 帧居中(行内字节偏移 4)合成为 128x64 全屏帧再走 render 回调；首次进入 IDLE 立即上帧。
- 交互：`show` 置 `idle_hold=true` 暂停待机动画（保持所显帧），`play`/`stream` 结束后或 `stop` 时 `idle_resume()` 恢复。
- 编译通过（EXITCODE=0）；RODATA +1024B（帧数组入 Flash），RAM 未增。

## 13. 像素编辑器（G:\AIMonitor\像素编辑器\动画编辑器.html）
- 单文件纯前端，无依赖。**纯黑白**（彩色已移除），画布尺寸 16/32/64/64×32/128×64(设备)。
- 设备面板用 **Web Serial API**（Chrome/Edge，需 localhost/https 打开；file:// 下不可用）：连接→上传动画(分块+CRC)、发送当前帧、设备播放/停止、流式预览、状态/删除。
- 帧→设备 128×64 XBM：`frameToDeviceBytes`，**等比缩放居中**（任意画布尺寸），LSB 位序。
- 选择/移动：左键拖拽=框选(抬起结束)，选中区内拖拽=像素实时跟随，右键取消选择。
- 关键校验命令：
  - JS 语法/ID：`python C:\Users\90346\AppData\Local\Temp\opencode\check_js.py`（esprima）
  - 帧/CRC/.anm/PXA 逻辑：`check_logic.py` / `check_chain.py` / `check_pxa.py`
  - GIF 编码 vs PIL：`check_final.py`；解码 vs 真实 gif：`check_content.py`
- 注意：仓库里 `动画编辑器 - 副本.html` 是旧备份，别改错文件。

## 14. PXA 分享格式（git 友好）
- 纯文本，`;` 元数据行 + `#`=黑 `.`=白 像素行；`;PXA v1` 魔数、`;W= H= F= FPS= LOOP=`、`;NAME=`、`;---- FRAME N ----` 分隔。
- 任意尺寸，编辑器可导入/导出；Python 端 `host/pxa.py`；与 .anm 互转仅限 128×64。
- 规格见 `像素编辑器\PXA格式.md`。

## 15. GIF 编解码要点（容易再踩坑，改动需回归验证）
- **码宽增长时机**（最易错）：编码器用 `nextCode >= (1<<codeSize)+1`（比字典计数晚一个），与标准解码器"首个新码由第 2 码经 KwKwK 构建"对齐。改回 `>= (1<<codeSize)` 会导出坏文件。
- LZW 首个新码 = `clear+2`；位打包必须用无符号 `>>>`（有符号 `>>` 溢出）。
- 调色板补齐 2 的幂；min code size 与 LZW 一致；图像描述符 packed 含本地颜色表标志 `0x80|(bits-1)`。
- 解码端 `compositeAll()` 逐帧合成（部分帧/透明/处置 0/1/2/3），先合成再缩放。
- 回归：`check_final.py`（编码 vs PIL）、`check_content.py`（解码 vs 真实 gif）。

## 16. 导入图片设计（静态图片）
- 交互式「导入预览」：等比缩放居中 → 拖动移动 + 滚轮缩放(光标为中心) + ↔ 适应复位 → 应用写当前帧/取消。
- 二值化三种模式：
  - 填充(全局阈值)：`gray < T` 黑。
  - **自适应(局部对比)**：`gray < 局部均值 - C`（邻域约 1/6 画布），按局部明暗保留内部细节（照片/渐变适用）。
  - 轮廓(边缘 Sobel)：梯度幅值 > T 黑，出线稿。
- 空白区按白处理；GIF 导入走等比适应 + 阈值。

## 17. 下一步(推荐起点)
1. 真机验证：`transmit` 60帧 → `play` 循环；`show` 即时直显；`stream` 24fps 不卡；断线续传 + CRC。
2. 真机串口 921600，接线 SCL=2/SDA=0（当前 .ino 已配 ESP8266；AIMonitor-32 是旧 ESP32 原型，别混淆）。
3. 上传 60 帧素材：编辑器画 → 上传；或 `scr/` gif：`anm_builder.frames_from_gif` → `mon.transmit`。
4. 固件若再加代码注意 IRAM 已 92%，Flash 272KB 略超 250KB 目标。
