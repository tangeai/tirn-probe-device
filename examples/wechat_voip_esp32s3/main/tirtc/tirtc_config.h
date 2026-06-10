#pragma once

/* TiRTC 与微信 VoIP 示例参数, 接入时主要改这里. */

#ifndef TIRTC_SERVICE_ENDPOINT
#define TIRTC_SERVICE_ENDPOINT "http://ep-tirtc.tange365.com"
#endif

#ifndef TIRTC_DEVICE_ID
#define TIRTC_DEVICE_ID "your_device_id"
#endif

#ifndef TIRTC_DEVICE_SECRET_KEY
#define TIRTC_DEVICE_SECRET_KEY "your_device_secret"
#endif

/* 业务服务器 WebSocket 地址 */
#ifndef TIRTC_WX_VOIP_WS_URI
#define TIRTC_WX_VOIP_WS_URI "ws://openapidemo.tange365.com/tirtc/voip/device/ws"
#endif

/* 主动呼叫目标来自业务服务器授权缓存; 这里只配置小程序版本: 0 正式版, 1 体验版, 2 开发版. */
#ifndef TIRTC_WX_VOIP_ACTIVE_CALL_VERSION_TYPE
#define TIRTC_WX_VOIP_ACTIVE_CALL_VERSION_TYPE 1
#endif

/*
 * 协议和 SDK 回调排查开关: 0 关闭, 1 打开.
 * 打开后会打印 WS/HTTP/SDK 回调细节,仅建议排查问题时使用.
 */
#ifndef TIRTC_WX_VOIP_DEBUG_LOG
#define TIRTC_WX_VOIP_DEBUG_LOG 0
#endif
