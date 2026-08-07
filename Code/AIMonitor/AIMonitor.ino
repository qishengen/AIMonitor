#include <Arduino.h>
#include <U8g2lib.h>

#include "fs.h"
#include "player.h"
#include "protocol.h"

#define SCL 2
#define SDA 0
// #define SCL 22
// #define SDA 21
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R2, SCL, SDA, U8X8_PIN_NONE);

static void render_frame(const uint8_t* frame) {
  u8g2.drawXBMP(0, 0, 128, 64, frame);
  u8g2.sendBuffer();
}

// 开机动画：纯程序化绘制（无文件依赖），阻塞约 1.5s。
static void boot_animation(void) {
  const unsigned long DUR_MS = 1500;
  const unsigned long STEP_MS = 16;
  unsigned long t0 = millis();
  u8g2.setFont(u8g2_font_6x13_tf);
  for (;;) {
    unsigned long t = millis() - t0;
    if (t >= DUR_MS) break;
    float p = (float)t / DUR_MS;

    u8g2.clearBuffer();

    // 顶部：状态圈收缩
    int r = 22 - (int)(p * 16);
    u8g2.drawCircle(64, 22, r > 0 ? r : 0);
    u8g2.drawPixel(64, 22);

    // 品牌字
    u8g2.drawStr(37, 42, "AIMonitor");

    // 底部进度条 + 进度端奔跑方块
    u8g2.drawFrame(8, 52, 112, 8);
    int bw = (int)(p * 110);
    u8g2.drawBox(10, 54, bw, 4);
    int rx = (10 + bw < 114) ? 10 + bw : 114;
    u8g2.drawBox(rx, 52, 4, 8);

    u8g2.sendBuffer();
    delay(STEP_MS);
  }
}

void setup(void) {
  // 扩大内核串口 RX 缓冲：渲染/写 Flash 阻塞期间不丢帧数据（默认仅 256B）
  Serial.setRxBufferSize(4096);
  Serial.begin(921600);
  fs_init();
  player_init(render_frame, on_player_event);
  protocol_setup();
  u8g2.begin();
  boot_animation();
  u8g2.clearBuffer();
  u8g2.sendBuffer();
}

void loop(void) {
  protocol_poll();
  player_tick();
}
