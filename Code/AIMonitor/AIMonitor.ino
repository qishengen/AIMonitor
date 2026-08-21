#include <Arduino.h>
#include <U8g2lib.h>

#include "fs.h"
#include "net.h"
#include "player.h"
#include "protocol.h"
#include "transport.h"
#include "webcfg.h"

#define SCL 5
#define SDA 4

// #define SCL 2
// #define SDA 0
// #define SCL 22
// #define SDA 21
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R2, SCL, SDA, U8X8_PIN_NONE);

static void render_frame(const uint8_t* frame) {
  u8g2.drawXBMP(0, 0, 128, 64, frame);
  u8g2.sendBuffer();
}

// 待机信息屏：显示 WiFi 状态(模式)+名称 / 密码 / TCP 端口（AP 热点默认常开，屏上直接可查连接信息）。
static void draw_idle_info(bool blink) {
  char line[40];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x13_tf);

  if (blink) u8g2.drawBox(122, 2, 4, 4);  // 右上角呼吸指示点

  u8g2.drawStr(2, 12, "AIMonitor");

  // WiFi 状态（ap/sta/ap+sta）+ 名称（AP 开则热点名，否则 STA ssid）
  snprintf(line, sizeof(line), "%s %s", net_mode_str(),
           net_has_ap() ? net_ap_ssid_str() : net_ssid_str());
  u8g2.drawStr(2, 28, line);

  // AP 热点密码（空=开放热点；独立于 STA pass）
  if (net_ap_pass_str()[0]) snprintf(line, sizeof(line), "PASS %s", net_ap_pass_str());
  else strcpy(line, "PASS (open)");
  u8g2.drawStr(2, 44, line);

  snprintf(line, sizeof(line), "TCP %u  C:%d", (unsigned)net_port(), net_client_count());
  u8g2.drawStr(2, 60, line);

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
  net_setup();
  webcfg_setup();
  player_init(render_frame, on_player_event);
  player_set_idle_info_cb(draw_idle_info);
  protocol_setup();
  u8g2.begin();
  boot_animation();
  u8g2.clearBuffer();
  u8g2.sendBuffer();
}

void loop(void) {
  transport_poll();
  net_poll();
  webcfg_poll();
  player_tick();
  protocol_tick();
}
