#pragma once

/* 示例媒体:不接硬件,循环发送根目录 send_audio.wav 演示 TiRTC 音频收发. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wechat_voip_media_start(tirtc_conn_t hconn);
void wechat_voip_media_stop(tirtc_conn_t hconn);
esp_err_t wechat_voip_media_stop_wait(tirtc_conn_t hconn, uint32_t timeout_ms);
bool wechat_voip_media_is_running(void);
bool wechat_voip_media_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, const void *data);

#ifdef __cplusplus
}
#endif
