#pragma once

/* 微信 VoIP 会话:保存本次入会信息,驱动 WHIP 建连和通话状态. */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wechat_voip_session_handle_join_room(cJSON *payload, bool auto_answer);
esp_err_t wechat_voip_session_answer(void);
esp_err_t wechat_voip_session_reject_incoming(void);
bool wechat_voip_session_is_idle(void);
bool wechat_voip_session_is_closing(void);
bool wechat_voip_session_ready_for_next_call(bool log_detail);
void wechat_voip_session_dump_status(const char *reason);
void wechat_voip_session_maintenance(void);

bool wechat_voip_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len);
bool wechat_voip_session_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data);
bool wechat_voip_session_on_conn_error(tirtc_conn_t hconn, int error);
bool wechat_voip_session_on_disconnected(tirtc_conn_t hconn);

bool wechat_voip_session_cancel_by_room(const char *room_id);
void wechat_voip_session_hangup(void);

#ifdef __cplusplus
}
#endif
