/*
 * TiRTC 应用层入口:负责 SDK 上线,统一回调和微信 VoIP 会话路由.
 */
#include "tirtc_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "tiRTC.h"
#include "tirtc_config.h"
#include "wechat_voip_session.h"
#include "wechat_voip_trace.h"

static const char *TAG = "tirtc_app";

enum
{
    TIRTC_DEVICE_LICENSE_MAX_LEN = 192,
    TIRTC_CALLBACK_TASK_STACK = 12288,
    TIRTC_CALLBACK_TASK_PRIORITY = 6,
    TIRTC_CALLBACK_QUEUE_LEN = 24,
    TIRTC_CALLBACK_PAYLOAD_MAX = 128,
};

typedef enum
{
    TIRTC_APP_EVT_SYS,
    TIRTC_APP_EVT_CONN_ACCEPTED,
    TIRTC_APP_EVT_CONN_ERROR,
    TIRTC_APP_EVT_DISCONNECTED,
    TIRTC_APP_EVT_COMMAND,
    TIRTC_APP_EVT_REQUEST_KEY_FRAME,
    TIRTC_APP_EVT_SUBSCRIBE_AUDIO,
    TIRTC_APP_EVT_UNSUBSCRIBE_AUDIO,
    TIRTC_APP_EVT_SUBSCRIBE_VIDEO,
    TIRTC_APP_EVT_UNSUBSCRIBE_VIDEO,
} tirtc_app_event_type_t;

typedef struct
{
    tirtc_app_event_type_t type;
    tirtc_conn_t hconn;
    int error;
    int sys_event;
    uint32_t cmdw;
    uint32_t len;
    uint8_t stream_id;
    uint8_t payload[TIRTC_CALLBACK_PAYLOAD_MAX];
} tirtc_app_event_t;

typedef struct
{
    char license[TIRTC_DEVICE_LICENSE_MAX_LEN];
    bool sdk_initialized;
    bool sdk_started;
} tirtc_runtime_t;

static tirtc_runtime_t s_tirtc;
static QueueHandle_t s_callback_queue;
static TaskHandle_t s_callback_task;
static volatile uint32_t s_callback_drop_count;

static bool config_is_placeholder(const char *value, const char *placeholder)
{
    return value == NULL || value[0] == '\0' || strcmp(value, placeholder) == 0;
}

static const char *event_type_name(tirtc_app_event_type_t type)
{
    switch (type)
    {
    case TIRTC_APP_EVT_SYS:
        return "sys";
    case TIRTC_APP_EVT_CONN_ACCEPTED:
        return "conn_accepted";
    case TIRTC_APP_EVT_CONN_ERROR:
        return "conn_error";
    case TIRTC_APP_EVT_DISCONNECTED:
        return "disconnected";
    case TIRTC_APP_EVT_COMMAND:
        return "command";
    case TIRTC_APP_EVT_REQUEST_KEY_FRAME:
        return "request_key_frame";
    case TIRTC_APP_EVT_SUBSCRIBE_AUDIO:
        return "subscribe_audio";
    case TIRTC_APP_EVT_UNSUBSCRIBE_AUDIO:
        return "unsubscribe_audio";
    case TIRTC_APP_EVT_SUBSCRIBE_VIDEO:
        return "subscribe_video";
    case TIRTC_APP_EVT_UNSUBSCRIBE_VIDEO:
        return "unsubscribe_video";
    default:
        return "unknown";
    }
}

static const char *sys_event_name(int event)
{
    switch (event)
    {
    case TIRTC_EVENT_SYS_STARTED:
        return "started";
    case TIRTC_EVENT_SYS_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}

static void post_callback_event(const tirtc_app_event_t *event)
{
    if (event == NULL || s_callback_queue == NULL)
    {
        ++s_callback_drop_count;
        WX_VOIP_TRACEW(TAG, "SDK 回调入队失败: event=%p queue=%p drop=%lu",
                       event,
                       s_callback_queue,
                       (unsigned long)s_callback_drop_count);
        return;
    }

    WX_VOIP_TRACEI(TAG,
                   "SDK 回调入队: type=%s hconn=%p sys=%d cmdw=0x%08x len=%u stream=%u",
                   event_type_name(event->type),
                   event->hconn,
                   event->sys_event,
                   (unsigned)event->cmdw,
                   (unsigned)event->len,
                   event->stream_id);

    if (xQueueSend(s_callback_queue, event, 0) != pdPASS)
    {
        ++s_callback_drop_count;
        WX_VOIP_TRACEW(TAG,
                       "SDK 回调队列已满: type=%s drop=%lu",
                       event_type_name(event->type),
                       (unsigned long)s_callback_drop_count);
    }
}

static void handle_callback_event(const tirtc_app_event_t *event)
{
    if (event == NULL)
    {
        return;
    }

    WX_VOIP_TRACEI(TAG,
                   "处理 SDK 回调: type=%s hconn=%p sys=%d cmdw=0x%08x len=%u stream=%u",
                   event_type_name(event->type),
                   event->hconn,
                   event->sys_event,
                   (unsigned)event->cmdw,
                   (unsigned)event->len,
                   event->stream_id);

    switch (event->type)
    {
    case TIRTC_APP_EVT_SYS:
        if (event->sys_event == TIRTC_EVENT_SYS_STARTED)
        {
            ESP_LOGI(TAG, "TiRTC 已上线,等待微信来电");
        }
        else if (event->sys_event == TIRTC_EVENT_SYS_STOPPED)
        {
            ESP_LOGI(TAG, "TiRTC 已停止");
        }
        break;

    case TIRTC_APP_EVT_CONN_ACCEPTED:
        ESP_LOGW(TAG, "收到非微信通话连接,已释放");
        if (event->hconn != NULL)
        {
            (void)TiRtcDisconnect(event->hconn);
        }
        break;

    case TIRTC_APP_EVT_CONN_ERROR:
        if (!wechat_voip_session_on_conn_error(event->hconn, event->error))
        {
            ESP_LOGE(TAG, "TiRTC 连接错误: %d %s", event->error, TiRtcGetErrorStr(event->error));
            if (event->hconn != NULL)
            {
                (void)TiRtcDisconnect(event->hconn);
            }
        }
        break;

    case TIRTC_APP_EVT_DISCONNECTED:
        if (!wechat_voip_session_on_disconnected(event->hconn))
        {
            ESP_LOGI(TAG, "TiRTC 连接已断开");
        }
        break;

    case TIRTC_APP_EVT_COMMAND:
        if (!wechat_voip_session_on_command(event->hconn,
                                            event->cmdw,
                                            event->len > 0 ? event->payload : NULL,
                                            event->len))
        {
            ESP_LOGD(TAG, "忽略非当前通话命令 hconn=%p cmdw=0x%08x len=%u",
                     event->hconn, (unsigned)event->cmdw, (unsigned)event->len);
        }
        break;

    case TIRTC_APP_EVT_REQUEST_KEY_FRAME:
        ESP_LOGD(TAG, "对端请求关键帧 hconn=%p stream=%u,本示例无视频流",
                 event->hconn, event->stream_id);
        break;

    case TIRTC_APP_EVT_SUBSCRIBE_AUDIO:
        ESP_LOGD(TAG, "对端订阅音频 hconn=%p stream=%u", event->hconn, event->stream_id);
        break;

    case TIRTC_APP_EVT_UNSUBSCRIBE_AUDIO:
        ESP_LOGD(TAG, "对端取消订阅音频 hconn=%p stream=%u", event->hconn, event->stream_id);
        break;

    case TIRTC_APP_EVT_SUBSCRIBE_VIDEO:
        ESP_LOGD(TAG, "对端订阅视频 hconn=%p stream=%u,本示例无视频流",
                 event->hconn, event->stream_id);
        break;

    case TIRTC_APP_EVT_UNSUBSCRIBE_VIDEO:
        ESP_LOGD(TAG, "对端取消订阅视频 hconn=%p stream=%u", event->hconn, event->stream_id);
        break;

    default:
        break;
    }
}

static void callback_task(void *arg)
{
    (void)arg;
    uint32_t last_drop_count = 0;
    tirtc_app_event_t event;

    while (true)
    {
        if (xQueueReceive(s_callback_queue, &event, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        uint32_t drop_count = s_callback_drop_count;
        if (drop_count != last_drop_count)
        {
            ESP_LOGW(TAG, "TiRTC 回调事件丢弃 %lu 个", (unsigned long)(drop_count - last_drop_count));
            last_drop_count = drop_count;
        }

        handle_callback_event(&event);
    }
}

static esp_err_t ensure_callback_worker(void)
{
    if (s_callback_queue == NULL)
    {
        s_callback_queue = xQueueCreate(TIRTC_CALLBACK_QUEUE_LEN, sizeof(tirtc_app_event_t));
        if (s_callback_queue == NULL)
        {
            ESP_LOGE(TAG, "创建 TiRTC 回调队列失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_callback_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(callback_task,
                                         "tirtc_cb",
                                         TIRTC_CALLBACK_TASK_STACK,
                                         NULL,
                                         TIRTC_CALLBACK_TASK_PRIORITY,
                                         &s_callback_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_callback_task = NULL;
        ESP_LOGE(TAG, "创建 TiRTC 回调任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void tirtc_log_redirect(const char *log, uint32_t length)
{
#if TIRTC_WX_VOIP_DEBUG_LOG
    ESP_LOGI("TiRTC", "%.*s", (int)length, log);
#else
    ESP_LOGD("TiRTC", "%.*s", (int)length, log);
#endif
}

static void on_event(int event, const void *data, int len)
{
    (void)data;

    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_event: event=%d(%s) len=%d",
                   event,
                   sys_event_name(event),
                   len);

    switch (event)
    {
    case TIRTC_EVENT_SYS_STARTED:
        s_tirtc.sdk_started = true;
        break;

    case TIRTC_EVENT_SYS_STOPPED:
        s_tirtc.sdk_started = false;
        break;

    default:
        break;
    }

    tirtc_app_event_t app_event = {
        .type = TIRTC_APP_EVT_SYS,
        .sys_event = event,
    };
    post_callback_event(&app_event);
}

static void on_conn_accepted(tirtc_conn_t hconn)
{
    WX_VOIP_TRACEI(TAG, "SDK 回调 on_conn_accepted: hconn=%p", hconn);

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_CONN_ACCEPTED,
        .hconn = hconn,
    };
    post_callback_event(&event);
}

static void on_conn_error(tirtc_conn_t hconn, int error)
{
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_conn_error: hconn=%p error=%d %s",
                   hconn,
                   error,
                   TiRtcGetErrorStr(error));

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_CONN_ERROR,
        .hconn = hconn,
        .error = error,
    };
    post_callback_event(&event);
}

static void on_disconnected(tirtc_conn_t hconn)
{
    WX_VOIP_TRACEI(TAG, "SDK 回调 on_disconnected: hconn=%p", hconn);

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_DISCONNECTED,
        .hconn = hconn,
    };
    post_callback_event(&event);
}

static void on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
#if TIRTC_WX_VOIP_DEBUG_LOG
    static uint32_t audio_count;
    ++audio_count;
    if (audio_count == 1 || (audio_count % 50U) == 0)
    {
        WX_VOIP_TRACEI(TAG,
                       "SDK 回调 on_audio: hconn=%p stream=%u media=%u flags=0x%08x len=%u ts=%u count=%lu",
                       hconn,
                       info ? info->stream_id : 0,
                       info ? info->media : 0,
                       info ? (unsigned)info->flags : 0,
                       info ? (unsigned)info->length : 0,
                       info ? (unsigned)info->ts : 0,
                       (unsigned long)audio_count);
    }
#endif

    if (!wechat_voip_session_on_audio(hconn, info, data) && info != NULL)
    {
        WX_VOIP_TRACEI(TAG,
                       "收到非当前通话音频: hconn=%p stream=%u len=%u",
                       hconn,
                       info->stream_id,
                       (unsigned)info->length);
    }
}

static void on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_video: hconn=%p stream=%u media=%u flags=0x%08x len=%u ts=%u",
                   hconn,
                   info ? info->stream_id : 0,
                   info ? info->media : 0,
                   info ? (unsigned)info->flags : 0,
                   info ? (unsigned)info->length : 0,
                   info ? (unsigned)info->ts : 0);
}

static void on_message(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_message: hconn=%p stream=%u media=%u flags=0x%08x len=%u ts=%u",
                   hconn,
                   info ? info->stream_id : 0,
                   info ? info->media : 0,
                   info ? (unsigned)info->flags : 0,
                   info ? (unsigned)info->length : 0,
                   info ? (unsigned)info->ts : 0);
}

static void on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_command: hconn=%p cmdw=0x%08x len=%u copy=%u",
                   hconn,
                   (unsigned)cmdw,
                   (unsigned)len,
                   (unsigned)(len > TIRTC_CALLBACK_PAYLOAD_MAX ? TIRTC_CALLBACK_PAYLOAD_MAX : len));

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_COMMAND,
        .hconn = hconn,
        .cmdw = cmdw,
    };
    if (data != NULL && len > 0)
    {
        event.len = len > TIRTC_CALLBACK_PAYLOAD_MAX ? TIRTC_CALLBACK_PAYLOAD_MAX : len;
        memcpy(event.payload, data, event.len);
    }
    post_callback_event(&event);
}

static void on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id)
{
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_request_key_frame: hconn=%p stream=%u",
                   hconn,
                   stream_id);

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_REQUEST_KEY_FRAME,
        .hconn = hconn,
        .stream_id = stream_id,
    };
    post_callback_event(&event);
}

static int on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_subscribe_audio: hconn=%p stream=%u",
                   hconn,
                   stream_id);

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_SUBSCRIBE_AUDIO,
        .hconn = hconn,
        .stream_id = stream_id,
    };
    post_callback_event(&event);
    return 0;
}

static void on_unsubscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_unsubscribe_audio: hconn=%p stream=%u",
                   hconn,
                   stream_id);

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_UNSUBSCRIBE_AUDIO,
        .hconn = hconn,
        .stream_id = stream_id,
    };
    post_callback_event(&event);
}

static int on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_subscribe_video: hconn=%p stream=%u",
                   hconn,
                   stream_id);

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_SUBSCRIBE_VIDEO,
        .hconn = hconn,
        .stream_id = stream_id,
    };
    post_callback_event(&event);
    return 0;
}

static void on_unsubscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    WX_VOIP_TRACEI(TAG,
                   "SDK 回调 on_unsubscribe_video: hconn=%p stream=%u",
                   hconn,
                   stream_id);

    tirtc_app_event_t event = {
        .type = TIRTC_APP_EVT_UNSUBSCRIBE_VIDEO,
        .hconn = hconn,
        .stream_id = stream_id,
    };
    post_callback_event(&event);
}

static const TIRTCCALLBACKS s_callbacks = {
    .on_event = on_event,
    .on_conn_accepted = on_conn_accepted,
    .on_conn_error = on_conn_error,
    .on_disconnected = on_disconnected,
    .on_audio = on_audio,
    .on_video = on_video,
    .on_message = on_message,
    .on_command = on_command,
    .on_request_key_frame = on_request_key_frame,
    .on_subscribe_audio = on_subscribe_audio,
    .on_unsubscribe_audio = on_unsubscribe_audio,
    .on_subscribe_video = on_subscribe_video,
    .on_unsubscribe_video = on_unsubscribe_video,
};

esp_err_t tirtc_start(void)
{
    if (s_tirtc.sdk_initialized)
    {
        ESP_LOGW(TAG, "TiRTC 已初始化,忽略重复启动");
        return ESP_OK;
    }

    if (config_is_placeholder(TIRTC_DEVICE_ID, "your_device_id") ||
        config_is_placeholder(TIRTC_DEVICE_SECRET_KEY, "your_device_secret"))
    {
        ESP_LOGE(TAG, "请先在 main/tirtc/tirtc_config.h 配置设备 ID 和设备密钥");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t worker_ret = ensure_callback_worker();
    if (worker_ret != ESP_OK)
    {
        return worker_ret;
    }

    ESP_LOGI(TAG, "TiRTC 版本: %s", TiRtcGetVersion());
    ESP_LOGI(TAG, "PSRAM: total=%zu free=%zu",
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    WX_VOIP_TRACEI(TAG,
                   "TiRTC 启动参数: endpoint=%s device_id=%s",
                   TIRTC_SERVICE_ENDPOINT,
                   TIRTC_DEVICE_ID);

    TiRtcLogSetCallback(tirtc_log_redirect);
    TiRtcLogSetLevel(0);

    int rc = TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                            TIRTC_SERVICE_ENDPOINT,
                            (uint32_t)strlen(TIRTC_SERVICE_ENDPOINT) + 1U);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC 服务地址失败: %d %s", rc, TiRtcGetErrorStr(rc));
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "TiRTC 服务地址: %s", TIRTC_SERVICE_ENDPOINT);

    WX_VOIP_TRACEI(TAG, "调用 TiRtcInit");
    rc = TiRtcInit();
    if (rc != 0)
    {
        ESP_LOGE(TAG, "TiRTC 初始化失败: %d %s", rc, TiRtcGetErrorStr(rc));
        return ESP_FAIL;
    }
    s_tirtc.sdk_initialized = true;

    int written = snprintf(s_tirtc.license,
                           sizeof(s_tirtc.license),
                           "%s,%s",
                           TIRTC_DEVICE_ID,
                           TIRTC_DEVICE_SECRET_KEY);
    if (written < 0 || written >= (int)sizeof(s_tirtc.license))
    {
        TiRtcUninit();
        s_tirtc = (tirtc_runtime_t){0};
        ESP_LOGE(TAG, "设备授权信息过长");
        return ESP_ERR_INVALID_SIZE;
    }

    WX_VOIP_TRACEI(TAG, "调用 TiRtcStart: license_len=%u", (unsigned)strlen(s_tirtc.license));
    rc = TiRtcStart(s_tirtc.license, &s_callbacks);
    if (rc != 0)
    {
        TiRtcUninit();
        s_tirtc = (tirtc_runtime_t){0};
        ESP_LOGE(TAG, "TiRTC 启动失败: %d %s", rc, TiRtcGetErrorStr(rc));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TiRTC 启动请求已提交");
    return ESP_OK;
}
