#pragma once

#define WEB_PORT 80
#define WEB_MAX_CLIENTS 4   // 网页并发请求（首页 4 个并行 fetch），槽位越多越不易丢

void webcfg_setup(void);
void webcfg_poll(void);   // 主循环高频调用：HTTP 门户（配网 + 控制面板 + /status + /cmd）

bool webcfg_running(void);      // 门户是否已启动（net_apply 后自动重绑，保持常开）
int webcfg_client_count(void);  // 当前连接的 Web 客户端数
