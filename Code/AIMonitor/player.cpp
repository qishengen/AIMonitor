#include <Arduino.h>
#include <string.h>
#include "player.h"
#include "fs.h"
#include "idle_anim.h"

#define FB_BYTES 1024

enum { ST_IDLE,
       ST_PLAY,
       ST_STREAM,
       ST_HOME };

static frame_cb render_cb = NULL;
static event_cb ev_cb = NULL;
static idle_info_cb idle_info = NULL;

static int st = ST_IDLE;
static char anim_name[64] = "";
static uint8_t fb[2][FB_BYTES];
static int cur = 0;  // 当前上屏缓冲
static int fps = 0;
static uint32_t period_ms = 0;
static bool looping = false;
static uint32_t next_due = 0;
static uint32_t seq = 0;    // 已播帧数
static uint32_t total = 0;  // stream 总帧数，0=不限
static bool pending_frame = false;

// ---- 内置待机动画（IDLE 状态循环，无文件依赖） ----
#define IDLE_FRAME_MS 700
static bool idle_hold = false;  // show 后暂停，避免覆盖已显帧
static uint16_t idle_cur = 0;
static uint32_t idle_next = 0;

// ---- 内置主页动画（home 命令，独立于 IDLE 信息屏） ----
static uint16_t home_cur = 0;
static uint32_t home_next = 0;

void player_init(frame_cb render, event_cb ev) {
  render_cb = render;
  ev_cb = ev;
  idle_cur = 0;
  idle_next = 0;  // 首次进入 IDLE 立即上第一帧
  idle_hold = false;
}

void player_set_idle_info_cb(idle_info_cb cb) {
  idle_info = cb;
}

uint8_t* player_recv_buf(void) {
  return fb[cur ^ 1];
}

static void idle_resume(void) {
  idle_hold = false;
  idle_cur = 0;
  idle_next = 0;  // 回到 IDLE 立即续播待机动画
}

static void finish_play(void) {
  st = ST_IDLE;
  anim_name[0] = 0;
  idle_resume();
  if (ev_cb) ev_cb("done", seq, fps, 0);
}

static void finish_stream(void) {
  st = ST_IDLE;
  idle_resume();
  if (ev_cb) ev_cb("done", seq, fps, 0);
}

void player_show_now(const uint8_t* src) {
  if (st == ST_HOME) st = ST_IDLE;  // home 状态下 show 回到 idle-hold
  if (src != fb[cur ^ 1]) memcpy(fb[cur ^ 1], src, FB_BYTES);
  cur ^= 1;
  if (render_cb) render_cb(fb[cur]);
  idle_hold = true;  // show 的帧保持显示，暂停待机动画
}

void player_stream_submit(const uint8_t* src) {
  if (st != ST_STREAM) return;
  if (src != fb[cur ^ 1]) memcpy(fb[cur ^ 1], src, FB_BYTES);
  pending_frame = true;
}

bool player_play(const char* name, int fpsv, bool lp) {
  if (!fs_anim_open(name)) return false;
  st = ST_IDLE;
  strncpy(anim_name, name, sizeof(anim_name) - 1);
  anim_name[sizeof(anim_name) - 1] = 0;
  int f = (fpsv > 0) ? fpsv : (int)fs_anim_fps();
  if (f < 1) f = 24;
  if (f > 60) f = 60;
  fps = f;
  period_ms = 1000 / f;
  looping = lp;
  seq = 0;
  pending_frame = false;
  if (fs_anim_count() == 0) return false;
  if (!fs_anim_read_frame(0, fb[cur ^ 1])) return false;
  cur ^= 1;
  if (render_cb) render_cb(fb[cur]);
  next_due = millis() + period_ms;
  st = ST_PLAY;
  return true;
}

void player_stop(void) {
  st = ST_IDLE;
  anim_name[0] = 0;
  pending_frame = false;
  idle_resume();
}

static void idle_render(uint16_t idx);  // 前置声明（实现位于文件后部）

bool player_play_home(void) {
  st = ST_HOME;
  anim_name[0] = 0;
  home_cur = 0;
  home_next = 0;
  idle_hold = false;
  idle_render(home_cur);
  home_cur = (uint16_t)((home_cur + 1) % IDLE_ANIM_FRAME_COUNT);
  home_next = millis() + IDLE_FRAME_MS;
  return true;
}

bool player_stream_start(int fpsv, bool lp, int totalv) {
  st = ST_IDLE;
  fps = fpsv;
  if (fps < 1) fps = 1;
  if (fps > 60) fps = 60;
  period_ms = 1000 / fps;
  looping = lp;
  total = (totalv > 0) ? (uint32_t)totalv : 0;
  seq = 0;
  pending_frame = false;
  anim_name[0] = 0;
  next_due = millis() + period_ms;
  st = ST_STREAM;
  return true;
}

void player_stream_end(void) {
  if (st == ST_STREAM) finish_stream();
}

static void play_tick(void) {
  uint32_t now = millis();
  if ((int32_t)(now - next_due) < 0) return;
  uint16_t fc = fs_anim_count();
  uint32_t idx = (fc > 0) ? (seq % fc) : 0;
  if (fs_anim_read_frame((uint16_t)idx, fb[cur ^ 1])) {
    cur ^= 1;
    seq++;
    if (render_cb) render_cb(fb[cur]);
    if (ev_cb) ev_cb("progress", idx, fps, 0);
    if (fc > 0 && seq >= fc) {
      if (looping) seq = 0;
      else {
        finish_play();
        return;
      }
    }
  }
  next_due = now + period_ms;
}

static void stream_tick(void) {
  if (!pending_frame) return;
  uint32_t now = millis();
  if ((int32_t)(now - next_due) < 0) return;
  cur ^= 1;
  seq++;
  pending_frame = false;
  if (render_cb) render_cb(fb[cur]);
  if (ev_cb) ev_cb("progress", seq - 1, fps, 0);
  if (total > 0 && seq >= total) {
    finish_stream();
    return;
  }
  next_due = now + period_ms;
}

// 将 64x64 待机帧居中合成为 128x64 帧（行内字节偏移 4）。
static void idle_render(uint16_t idx) {
  uint8_t* out = fb[cur ^ 1];
  memset(out, 0, FB_BYTES);
  const uint8_t* src = idle_anim[idx];
  for (int y = 0; y < IDLE_ANIM_H; y++)
    memcpy(out + y * 16 + 4, src + y * (IDLE_ANIM_W / 8), IDLE_ANIM_W / 8);
  cur ^= 1;
  if (render_cb) render_cb(fb[cur]);
}

static void idle_tick(void) {
  if (idle_hold) return;
  uint32_t now = millis();
  if ((int32_t)(now - idle_next) < 0) return;
  if (idle_info) {
    idle_info((idle_cur & 1) != 0);  // 信息屏：每 700ms 重绘，blink 呼吸指示
    idle_cur++;
  } else {
    idle_render(idle_cur);
    idle_cur = (uint16_t)((idle_cur + 1) % IDLE_ANIM_FRAME_COUNT);
  }
  idle_next = now + IDLE_FRAME_MS;
}

static void home_tick(void) {
  uint32_t now = millis();
  if ((int32_t)(now - home_next) < 0) return;
  idle_render(home_cur);
  home_cur = (uint16_t)((home_cur + 1) % IDLE_ANIM_FRAME_COUNT);
  home_next = now + IDLE_FRAME_MS;
}

void player_tick(void) {
  if (st == ST_PLAY) play_tick();
  else if (st == ST_STREAM) stream_tick();
  else if (st == ST_HOME) home_tick();
  else idle_tick();
}

const char* player_state_str(void) {
  switch (st) {
    case ST_PLAY: return "play";
    case ST_STREAM: return "stream";
    case ST_HOME: return "home";
    default: return "idle";
  }
}

const char* player_anim_name(void) {
  return anim_name;
}
uint32_t player_seq(void) {
  return seq;
}
int player_fps(void) {
  return fps;
}
