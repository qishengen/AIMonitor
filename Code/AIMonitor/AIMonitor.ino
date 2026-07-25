#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "IMG.h"


#define SCL 2
#define SDA 0
// #define SCL 22
// #define SDA 21
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, SCL, SDA, U8X8_PIN_NONE);  // All Boards without Reset of the Display




void setup(void) {
  Serial.begin(9600);
  u8g2.begin();
}



void loop(void) {
  for (int i = 0; i < FRAME_COUNT; i++) {
    Serial.printf("i:%d\n", i);
    Serial.println("hello world");
    u8g2.setFont(u8g2_font_8x13_me);
    u8g2.drawStr(0, 10, "11°");
    u8g2.drawXBMP(32, 0, WIDTH, HEIGHT, animation[i]);
    u8g2.sendBuffer();
    delay(500);
  }
}
