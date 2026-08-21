#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "status.h"
#include "version.h"
#include "net.h"
#include "fs.h"
#include "player.h"

uint32_t status_uptime(void) { return millis() / 1000u; }

int status_snapshot(char* out, int outsz) {
  if (outsz < 32) return 0;
  int pos = 0;
#define APPEND(...) do { \
    int _w = snprintf(out + pos, (size_t)(outsz - pos > 0 ? outsz - pos : 0), __VA_ARGS__); \
    if (_w < 0) { out[pos] = 0; return pos; } \
    pos += _w; \
    if (pos >= outsz) pos = outsz - 1; \
  } while (0)

  APPEND("{\"fw\":\"%s\",\"proto\":%d,\"uptime\":%lu,\"serial\":{\"baud\":%u},",
         FW_VERSION, PROTO_VERSION, (unsigned long)status_uptime(), (unsigned)SERIAL_BAUD);
  APPEND("\"net\":{\"mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"ap_secured\":%d,",
         net_mode_str(), net_ap_ssid_str(), net_ap_ip_str(), net_ap_pass_str()[0] ? 1 : 0);
  APPEND("\"sta_ssid\":\"%s\",\"sta_connected\":%d,\"sta_ip\":\"%s\",\"rssi\":%d,",
         net_ssid_str(), net_sta_connected() ? 1 : 0, net_sta_ip_str(), net_rssi());
  APPEND("\"tcp_port\":%u,\"tcp_clients\":%d},",
         (unsigned)net_port(), net_client_count());
  APPEND("\"fs\":{\"total\":%lu,\"free\":%lu,\"anims\":",
         (unsigned long)fs_total_bytes(), (unsigned long)fs_free_bytes());
  fs_list_array(out + pos, outsz - pos);
  pos += (int)strlen(out + pos);
  APPEND("},\"player\":{\"state\":\"%s\",\"name\":\"%s\",\"seq\":%lu,\"fps\":%d}}",
         player_state_str(), player_anim_name(), (unsigned long)player_seq(), player_fps());
#undef APPEND
  return pos;
}
