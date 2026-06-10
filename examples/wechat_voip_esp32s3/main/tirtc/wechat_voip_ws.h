#pragma once

/* 业务 WebSocket:上报媒体能力,同步授权联系人,转发微信 VoIP 入会消息. */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wechat_voip_ws_start(void);
bool wechat_voip_ws_is_connected(void);
esp_err_t wechat_voip_request_call(void);
bool wechat_voip_request_call_busy(void);
void wechat_voip_cancel_pending_call(void);
void wechat_voip_ws_maintenance(void);

#ifdef __cplusplus
}
#endif
