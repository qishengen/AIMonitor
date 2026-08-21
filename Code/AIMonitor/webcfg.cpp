#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <ESP8266WiFi.h>
#include "net.h"
#include "webcfg.h"
#include "transport.h"
#include "protocol.h"
#include "status.h"

// ---------------- 请求缓冲 ----------------

typedef struct {
  WiFiClient c;
  char buf[1536];
  int len;
  uint32_t last;
} webc_t;

static WiFiServer g_ws(WEB_PORT);
static webc_t g_web[WEB_MAX_CLIENTS];
static uint32_t g_ws_gen = 0;  // 上次重绑时 net_generation
static bool g_ws_on = false;   // 门户是否已绑定

// ---------------- 小工具 ----------------

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_ip(const char* s, uint8_t* out) {
  unsigned a[4];
  if (sscanf(s, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) != 4) return 0;
  for (int i = 0; i < 4; i++) if (a[i] > 255) return 0;
  for (int i = 0; i < 4; i++) out[i] = (uint8_t)a[i];
  return 1;
}

static bool prefix_ci(const char* s, const char* p) {
  while (*p) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false;
    s++; p++;
  }
  return true;
}

static void url_decode(char* dst, const char* src, int n) {
  int k = 0;
  for (int i = 0; src[i] && i < n; i++) {
    char c = src[i];
    if (c == '+') {
      dst[k++] = ' ';
    } else if (c == '%' && hexval(src[i+1]) >= 0 && hexval(src[i+2]) >= 0) {
      dst[k++] = (char)((hexval(src[i+1]) << 4) | hexval(src[i+2]));
      i += 2;
    } else {
      dst[k++] = c;
    }
  }
  dst[k] = 0;
}

// 从 urlencoded body 中取 key 对应的值（解码后写入 out）
static void form_value(const char* body, const char* key, char* out, int n) {
  int kl = (int)strlen(key);
  out[0] = 0;
  const char* s = body;
  while (s && *s) {
    if (strncmp(s, key, (size_t)kl) == 0 && s[kl] == '=') {
      const char* v = s + kl + 1;
      const char* e = strchr(v, '&');
      int vn = e ? (int)(e - v) : (int)strlen(v);
      char tmp[200];
      if (vn > (int)sizeof(tmp) - 1) vn = (int)sizeof(tmp) - 1;
      memcpy(tmp, v, (size_t)vn);
      tmp[vn] = 0;
      url_decode(out, tmp, n);
      return;
    }
    s = strchr(s, '&');
    if (s) s++;
  }
}

// ---------------- HTTP 解析 ----------------

// 返回 header 结束位置（内容体起点），未找到返回 -1
static int header_end(const char* buf, int len) {
  for (int i = 0; i <= len - 4; i++)
    if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') return i + 4;
  for (int i = 0; i <= len - 2; i++)
    if (buf[i] == '\n' && buf[i+1] == '\n') return i + 2;
  return -1;
}

static int content_length(const char* buf, int he) {
  int i = 0;
  while (i < he) {
    if (prefix_ci(buf + i, "Content-Length:")) {
      const char* v = buf + i + 15;
      while (*v == ' ' || *v == '\t') v++;
      return atoi(v);
    }
    while (i < he && buf[i] != '\n') i++;
    if (i < he) i++;
  }
  return 0;
}

static void get_path(const char* reqline, char* out, int n) {
  const char* s = reqline;
  while (*s && *s != ' ') s++;
  while (*s == ' ') s++;
  int k = 0;
  while (*s && *s != ' ' && *s != '?' && k < n - 1) out[k++] = *s++;
  out[k] = 0;
}

// ---------------- 响应 ----------------

static void send_bytes(WiFiClient* c, const char* s, size_t len) {
  size_t off = 0;
  while (off < len) {
    size_t w = c->write((const uint8_t*)s + off, len - off);
    if (w == 0) break;
    off += w;
  }
}

static const char* status_text(int code) {
  if (code == 200) return "OK";
  if (code == 404) return "Not Found";
  if (code == 413) return "Request Entity Too Large";
  return "Bad Request";
}

static void respond(WiFiClient* c, int code, const char* ct, const char* body) {
  char hdr[160];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
                   code, status_text(code), ct, (unsigned)strlen(body));
  send_bytes(c, hdr, (size_t)n);
  send_bytes(c, body, strlen(body));
}

static void respond_html(WiFiClient* c, int code, const char* body) {
  respond(c, code, "text/html; charset=utf-8", body);
}

static void respond_json(WiFiClient* c, const char* body) {
  respond(c, 200, "application/json; charset=utf-8", body);
}

static void respond_text(WiFiClient* c, int code, const char* body) {
  respond(c, code, "text/plain; charset=utf-8", body);
}

// ---------------- 页面与路由 ----------------

static const char INDEX_HTML[] =
  "<!doctype html><html lang='zh'><head><meta charset='utf-8'><meta name='viewport' "
  "content='width=device-width,initial-scale=1'><title>AIMonitor 控制面板</title><style>"
  "body{font-family:system-ui,sans-serif;max-width:600px;margin:16px auto;padding:0 12px;"
  "background:#111;color:#eee}h1{font-size:20px;margin:8px 0}"
  ".card{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:10px 12px;margin:10px 0}"
  ".card h2{font-size:14px;margin:0 0 8px;color:#9cf}"
  "table{width:100%;border-collapse:collapse;font-size:13px}td{padding:3px 4px;border-bottom:1px solid #222}"
  "td.k{color:#888;width:120px}"
  "button{padding:8px 12px;margin:4px;font-size:14px;border-radius:6px;border:none;"
  "background:#2d6cdf;color:#fff;font-weight:600;cursor:pointer}"
  "button.danger{background:#c0392b}button.small{padding:3px 8px;font-size:12px;margin:2px}"
  "input,select{width:100%;box-sizing:border-box;padding:8px;margin:4px 0;font-size:14px;"
  "border-radius:6px;border:1px solid #444;background:#1e1e1e;color:#eee}"
  "label{font-size:12px;color:#aaa}.row{display:flex;gap:8px}.row>div{flex:1}"
  "#msg{white-space:pre-wrap;font-size:13px;margin-top:8px;color:#ffd}"
  "#fsbar{height:8px;background:#222;border-radius:4px;margin:4px 0}#fsfill{height:8px;"
  "background:#2d6cdf;border-radius:4px;width:0%}"
  "</style></head><body><h1>AIMonitor 控制面板</h1>"

  "<div class='card'><h2>网络状态</h2><table id='net'></table></div>"
  "<div class='card'><h2>播放器</h2><table id='player'></table></div>"
  "<div class='card'><h2>存储</h2><div id='fsbar'><div id='fsfill'></div></div>"
  "<div id='fsinfo'></div><div id='files'></div></div>"
  "<div class='card'><h2>系统</h2><table id='sys'></table></div>"

  "<div class='card'><h2>控制</h2>"
  "<button onclick='cmd({cmd:\"stop\"})'>待机</button>"
  "<button onclick='cmd({cmd:\"home\"})'>主页动画</button>"
  "<button onclick='cmd({cmd:\"wifi_reconnect\"})'>重连WiFi</button>"
  "<button class='danger' onclick='cmd({cmd:\"reset\"})'>重启设备</button>"
  "<button class='danger' onclick='cmd({cmd:\"factory\"})'>恢复出厂</button>"
  "<div id='msg'></div></div>"

  "<div class='card'><h2>WiFi 配网</h2>"
  "<form id='f' onsubmit='return save()'>"
  "<label>WiFi 模式</label><select id='mode'>"
  "<option value='sta'>STA（连接路由器）</option>"
  "<option value='ap'>AP（仅热点）</option>"
  "<option value='ap+sta'>AP+STA（两者都开）</option></select>"
  "<label>SSID</label><input id='ssid' list='aps' placeholder='选择或输入 WiFi 名称'>"
  "<datalist id='aps'></datalist>"
  "<label>密码</label><input id='pass' type='password' placeholder='WiFi 密码'>"
  "<label>AP 热点密码（≥8 位，留空=热点开放）</label><input id='ap_pass' type='password' placeholder='AP 热点密码'>"
  "<label>协议 TCP 端口</label><input id='port' type='number' value='8088' min='1' max='65535'>"
  "<label>静态 IP（可选，留空则 DHCP）</label>"
  "<div class='row'><div><input id='ip' placeholder='IP'></div><div><input id='gw' placeholder='网关'></div>"
  "<div><input id='mask' placeholder='掩码'></div></div>"
  "<button type='submit'>保存并连接</button></form></div>"

  "<script>"
  "function $(id){return document.getElementById(id);}"
  "function esc(s){return String(s).replace(/[&<>\"']/g,function(c){"
  "return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c];});}"
  "function refresh(){fetch('/status').then(function(r){return r.json();}).then(function(s){"
  "var n=s.net||{};$('net').innerHTML="
  "'<tr><td class=\"k\">模式</td><td>'+esc(n.mode||'')+'</td></tr>'+"
  "'<tr><td class=\"k\">AP热点</td><td>'+esc(n.ap_ssid||'')+' ('+esc(n.ap_ip||'')+')</td></tr>'+"
  "'<tr><td class=\"k\">AP密码</td><td>'+((n.ap_secured)?'已设置':'开放')+'</td></tr>'+"
  "'<tr><td class=\"k\">STA WiFi</td><td>'+esc(n.sta_ssid||'-')+' '"
  "+(n.sta_connected?'已连接':'未连接')+' '+esc(n.sta_ip||'')+' RSSI:'+n.rssi+'</td></tr>'+"
  "'<tr><td class=\"k\">TCP端口</td><td>'+n.tcp_port+' (客户端 '+n.tcp_clients+')</td></tr>';"
  "var p=s.player||{};$('player').innerHTML="
  "'<tr><td class=\"k\">状态</td><td>'+esc(p.state||'')+'</td></tr>'+"
  "'<tr><td class=\"k\">动画</td><td>'+esc(p.name||'-')+'</td></tr>'+"
  "'<tr><td class=\"k\">帧序</td><td>'+p.seq+'</td></tr>'+"
  "'<tr><td class=\"k\">帧率</td><td>'+p.fps+' fps</td></tr>';"
  "$('sys').innerHTML="
  "'<tr><td class=\"k\">固件</td><td>'+esc(s.fw||'')+' (proto '+(s.proto||0)+')</td></tr>'+"
"'<tr><td class=\"k\">运行时长</td><td>'+(s.uptime||0)+' s</td></tr>'+"
"'<tr><td class=\"k\">波特率</td><td>'+((s.serial||{}).baud||0)+'</td></tr>';"
  "var fs=s.fs||{},used=(fs.total||0)-(fs.free||0),pct=(fs.total)?Math.round(used*100/fs.total):0;"
  "$('fsfill').style.width=pct+'%';"
  "$('fsinfo').textContent='已用 '+used+' / '+fs.total+' B（空闲 '+fs.free+' B）';"
  "var files=fs.anims||[],h='';"
  "if(!files.length){h='<tr><td>（无动画文件）</td></tr>';}"
  "for(var i=0;i<files.length;i++){"
  "var nm=files[i],jm=JSON.stringify(nm);"
"h+='<tr><td>'+esc(nm)+'</td><td style=\"text-align:right\">'+"
"'<button class=\"small\" onclick=\\'cmd({cmd:\"play\",name:'+jm+'})\\'>播放</button> '+"
"'<button class=\"small danger\" onclick=\\'del('+jm+')\\'>删除</button></td></tr>';}"
  "$('files').innerHTML='<table>'+h+'</table>';"
  "}).catch(function(){});}"
  "function cmd(o){fetch('/cmd',{method:'POST',body:JSON.stringify(o)})"
  ".then(function(r){return r.text();}).then(function(t){$('msg').textContent=t||'ok';})"
  ".catch(function(){ $('msg').textContent='命令失败'; });}"
  "function del(n){if(confirm('删除 '+n+' ？')){cmd({cmd:'delete',name:n});}}"
  "fetch('/cfg').then(function(r){return r.json();}).then(function(c){"
  "$('mode').value=c.mode||'ap';$('ssid').value=c.ssid||'';$('port').value=c.port||8088;"
  "$('ip').value=c.ip||'';$('gw').value=c.gw||'';$('mask').value=c.mask||'';}).catch(function(){});"
  "fetch('/scan').then(function(r){return r.json();}).then(function(a){"
  "var d=$('aps');(a.aps||[]).forEach(function(sv){"
  "var o=document.createElement('option');o.value=sv;d.appendChild(o);});}).catch(function(){});"
  "function save(){var p=new URLSearchParams();"
  "p.set('mode',$('mode').value);p.set('ssid',$('ssid').value.trim());"
  "p.set('pass',$('pass').value);p.set('port',$('port').value);"
  "p.set('ip',$('ip').value.trim());p.set('gw',$('gw').value.trim());"
  "p.set('mask',$('mask').value.trim());p.set('ap_pass',$('ap_pass').value);"
  "$('msg').textContent='保存中…';"
  "fetch('/save',{method:'POST',body:p}).then(function(r){return r.text();}).then(function(t){"
  "$('msg').textContent=t;}).catch(function(){ $('msg').textContent='保存失败'; });return false;}"
  "refresh();setInterval(refresh,2000);"
  "</script></body></html>";

static void respond_cfg(WiFiClient* c) {
  net_cfg_t cfg;
  net_load_cfg(&cfg);
  char ips[16] = "", gws[16] = "", masks[16] = "";
  if (cfg.have_static) {
    snprintf(ips, sizeof(ips), "%u.%u.%u.%u", cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
    snprintf(gws, sizeof(gws), "%u.%u.%u.%u", cfg.gw[0], cfg.gw[1], cfg.gw[2], cfg.gw[3]);
    snprintf(masks, sizeof(masks), "%u.%u.%u.%u", cfg.mask[0], cfg.mask[1], cfg.mask[2], cfg.mask[3]);
  }
  char b[200];
  snprintf(b, sizeof(b),
           "{\"mode\":\"%s\",\"ssid\":\"%s\",\"pass\":\"\",\"port\":%u,"
           "\"ip\":\"%s\",\"gw\":\"%s\",\"mask\":\"%s\",\"ap_pass\":\"\"}",
           cfg.mode, cfg.ssid, (unsigned)cfg.port, ips, gws, masks);
  respond_json(c, b);
}

static void respond_scan(WiFiClient* c) {
  char b[768];
  int pos = snprintf(b, sizeof(b), "{\"aps\":[");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && pos < (int)sizeof(b) - 48; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    if (pos > 7) b[pos++] = ',';
    pos += snprintf(b + pos, sizeof(b) - pos, "\"%s\"", ssid.c_str());
  }
  WiFi.scanDelete();
  if (pos >= (int)sizeof(b) - 2) pos = sizeof(b) - 3;
  b[pos++] = ']';
  b[pos++] = '}';
  b[pos] = 0;
  respond_json(c, b);
}

static void respond_status(WiFiClient* c) {
  char b[1536];
  status_snapshot(b, sizeof(b));
  respond_json(c, b);
}

static void handle_cmd(webc_t* w, int he, int clen) {
  char body[256];
  int n = clen < 255 ? clen : 255;
  memcpy(body, w->buf + he, (size_t)n);
  body[n] = 0;

  char out[512];
  int on = protocol_web_exec(body, n, out, sizeof(out));
  if (on <= 0) {
    respond_text(&w->c, 503, "{\"resp\":\"busy\"}");
    return;
  }
  out[on] = 0;
  respond_text(&w->c, 200, out);
}

static void handle_save(webc_t* w, int he, int clen) {
  char body[256];
  int n = clen < 255 ? clen : 255;
  memcpy(body, w->buf + he, (size_t)n);
  body[n] = 0;

  char mode[16] = "", ssid[33] = "", pass[33] = "", port_s[16] = "";
  char ips[16] = "", gws[16] = "", masks[16] = "", ap[33] = "";
  form_value(body, "mode", mode, sizeof(mode));
  form_value(body, "ssid", ssid, sizeof(ssid));
  form_value(body, "pass", pass, sizeof(pass));
  form_value(body, "port", port_s, sizeof(port_s));
  form_value(body, "ip", ips, sizeof(ips));
  form_value(body, "gw", gws, sizeof(gws));
  form_value(body, "mask", masks, sizeof(masks));
  form_value(body, "ap_pass", ap, sizeof(ap));

  net_cfg_t cfg;
  net_load_cfg(&cfg);  // 以已存配置为基础
  if (mode[0] && (!strcmp(mode, "ap") || !strcmp(mode, "sta") || !strcmp(mode, "ap+sta"))) {
    strncpy(cfg.mode, mode, sizeof(cfg.mode) - 1);
    cfg.mode[sizeof(cfg.mode) - 1] = 0;
  }
  if (ssid[0]) { strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1); cfg.ssid[sizeof(cfg.ssid) - 1] = 0; }
  if (pass[0]) { strncpy(cfg.pass, pass, sizeof(cfg.pass) - 1); cfg.pass[sizeof(cfg.pass) - 1] = 0; }
  // AP 热点密码：非空须 ≥8 位；留空=开放热点
  if (ap[0]) {
    if (strlen(ap) < 8) {
      respond_text(&w->c, 400, "AP 热点密码至少 8 位，留空则热点开放。");
      return;
    }
    strncpy(cfg.ap_pass, ap, sizeof(cfg.ap_pass) - 1);
    cfg.ap_pass[sizeof(cfg.ap_pass) - 1] = 0;
  } else {
    cfg.ap_pass[0] = 0;
  }
  int p = atoi(port_s);
  if (p >= 1 && p <= 65535) cfg.port = (uint16_t)p;
  cfg.have_static = false;
  if (ips[0] && parse_ip(ips, cfg.ip)) {
    cfg.have_static = true;
    if (gws[0]) parse_ip(gws, cfg.gw);
    else { cfg.gw[0] = cfg.ip[0]; cfg.gw[1] = cfg.ip[1]; cfg.gw[2] = cfg.ip[2]; cfg.gw[3] = 1; }
    if (masks[0]) parse_ip(masks, cfg.mask);
    else { cfg.mask[0] = 255; cfg.mask[1] = 255; cfg.mask[2] = 255; cfg.mask[3] = 0; }
  }

  net_save_cfg(&cfg);
  // 先回包并稍等 lwip 发完，再切换网络（应用配置会断当前 AP/STA 连接）
  respond_text(&w->c, 200,
               "已保存。STA 模式请连接 WiFi 后访问 http://aimonitor.local:8088 或设备 IP:8088。");
  delay(150);
  net_apply(&cfg);
}

static void handle(webc_t* w, int he, int clen) {
  char path[64];
  get_path(w->buf, path, sizeof(path));
  if (path[0] == 0) { respond_text(&w->c, 400, "bad request"); return; }
  if (strcmp(path, "/save") == 0) {
    handle_save(w, he, clen);
  } else if (strcmp(path, "/cmd") == 0) {
    handle_cmd(w, he, clen);
  } else if (strcmp(path, "/status") == 0) {
    respond_status(&w->c);
  } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
    respond_html(&w->c, 200, INDEX_HTML);
  } else if (strcmp(path, "/cfg") == 0) {
    respond_cfg(&w->c);
  } else if (strcmp(path, "/scan") == 0) {
    respond_scan(&w->c);
  } else {
    respond_html(&w->c, 404, "<h1>404 Not Found</h1>");
  }
}

// ---------------- 轮询 ----------------

void webcfg_setup(void) {
  g_ws_gen = net_generation();
  g_ws.begin(WEB_PORT);
  g_ws_on = true;
}

bool webcfg_running(void) { return g_ws_on; }

int webcfg_client_count(void) {
  int n = 0;
  for (int i = 0; i < WEB_MAX_CLIENTS; i++)
    if (g_web[i].c && g_web[i].c.connected()) n++;
  return n;
}

void webcfg_poll(void) {
  // 门户常开：net_apply 切换 WiFi 后重绑 web server（AP/STA/ap+sta 任何模式均可达）
  if (net_generation() != g_ws_gen) {
    g_ws.close();
    g_ws.begin(WEB_PORT);
    g_ws_gen = net_generation();
    g_ws_on = true;
  }

  WiFiClient nc = g_ws.accept();
  if (nc) {
    for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
      if (!g_web[i].c) {
        g_web[i].c = nc;
        g_web[i].len = 0;
        g_web[i].last = millis();
        break;
      }
    }
  }
  for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
    webc_t* w = &g_web[i];
    if (!w->c) continue;
    if (!w->c.connected()) { w->c.stop(); continue; }
    while (w->c.available() && w->len < (int)sizeof(w->buf) - 1) {
      int b = w->c.read();
      if (b < 0) break;
      w->buf[w->len++] = (char)b;
      w->last = millis();
    }
    w->buf[w->len] = 0;
    int he = header_end(w->buf, w->len);
    if (he >= 0) {
      int clen = content_length(w->buf, he);
      if (w->len >= he + clen) {
        handle(w, he, clen);
        w->c.stop();
      }
      continue;  // 内容体未收齐，继续
    }
    if (w->len >= (int)sizeof(w->buf) - 1) {  // 头未收齐已溢出
      respond_text(&w->c, 413, "request too large");
      w->c.stop();
      continue;
    }
    if (millis() - w->last > 10000) w->c.stop();  // 10s 无进展则释放槽位
  }
}
