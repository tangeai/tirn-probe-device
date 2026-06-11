/*
 * 微信 VoIP 业务 WebSocket 示例.
 *
 * 连接业务服务器后,上报媒体能力,同步微信授权联系人,并把
 * 入会通知交给会话模块.设备主动呼叫也在这里发起.
 */
#include "wechat_voip_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "tirtc_config.h"
#include "wechat_voip_session.h"
#include "wechat_voip_trace.h"

static const char *TAG = "wx_voip_ws";
static const char *ACTIVE_CALL_ROOM_TYPE = "voice";
static const char *ACTIVE_CALL_SOURCE = "esp32s3_demo";
static portMUX_TYPE s_active_call_lock = portMUX_INITIALIZER_UNLOCKED;

enum
{
    WS_TASK_STACK = 16384,
    WS_TASK_PRIORITY = 5,
    WS_FRAG_BUF_SIZE = 8192,
    WS_MSG_QUEUE_LEN = 8,
    WS_MSG_TASK_STACK = 24576,
    WS_MSG_TASK_PRIORITY = 5,
    WS_PING_INTERVAL_MS = 30000,
    WS_RECONNECT_TIMEOUT_MS = 5000,
    ACTIVE_CALL_TASK_STACK = 16384,
    ACTIVE_CALL_TASK_PRIORITY = 5,
    ACTIVE_CALL_HTTP_TIMEOUT_MS = 3000,
    ACTIVE_CALL_HTTP_HARD_TIMEOUT_MS = 3500,
    ACTIVE_CALL_DNS_TIMEOUT_MS = 2500,
    ACTIVE_CALL_REQUEST_GUARD_MS = ACTIVE_CALL_HTTP_HARD_TIMEOUT_MS + 1000,
    ACTIVE_CALL_HTTP_RESP_SIZE = 512,
    ACTIVE_CALL_HTTP_HEAD_SIZE = 512,
    ACTIVE_CALL_HTTP_PORT_SIZE = 8,
    AUTH_SYNC_TASK_STACK = 8192,
    AUTH_SYNC_TASK_PRIORITY = 5,
    AUTH_SYNC_RETRY_COUNT = 5,
    AUTH_SYNC_RETRY_MS = 2000,
    WX_OPENID_MAX_LEN = 96,
    WX_MODEL_ID_MAX_LEN = 64,
    WX_APP_ID_MAX_LEN = 64,
    DEVICE_AUDIO_RATE = 8000,
    DEVICE_AUDIO_CHANNELS = 1,
    DEVICE_CALLING_TIMEOUT_SEC = 30,
    ACTIVE_CALL_JOIN_WAIT_MS = (DEVICE_CALLING_TIMEOUT_SEC + 5) * 1000,
};

static esp_websocket_client_handle_t s_client;
static SemaphoreHandle_t s_send_mutex;
static QueueHandle_t s_ws_msg_queue;
static TaskHandle_t s_ws_msg_task;
static TaskHandle_t s_active_call_worker_task;
static TaskHandle_t s_auth_sync_task;
static SemaphoreHandle_t s_dns_done_sem;
static portMUX_TYPE s_auth_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_msg_id_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_dns_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_connected;
static uint32_t s_msg_id;
static char s_uri[512];
static char *s_frag_buf;
static int s_frag_len;

typedef struct
{
    uint32_t seq;
    bool waiting;
    bool done;
    bool ok;
    ip_addr_t addr;
} dns_lookup_state_t;

typedef struct
{
    char *json;
    int len;
} ws_msg_item_t;

typedef struct
{
    char openid[WX_OPENID_MAX_LEN];
    char model_id[WX_MODEL_ID_MAX_LEN];
    char app_id[WX_APP_ID_MAX_LEN];
} wx_auth_user_t;

typedef enum
{
    ACTIVE_CALL_IDLE,
    ACTIVE_CALL_REQUESTING,
    ACTIVE_CALL_WAIT_JOIN,
} active_call_state_t;

static wx_auth_user_t s_cached_auth;
static active_call_state_t s_active_call_state;
static int64_t s_active_call_deadline_us;
static uint32_t s_active_call_seq;
static char s_active_call_stage[48];
static dns_lookup_state_t s_dns_lookup;
static uint32_t s_dns_lookup_seq;
static bool s_dns_cache_valid;
static char s_dns_cache_host[128];
static ip_addr_t s_dns_cache_addr;

/*
 * 主动呼叫分两段:
 * 1. HTTP POST /device/call 只负责让微信侧响铃.
 * 2. 用户接听后,业务 WS 会回推 wx_join_voip_room,设备再自动进入 WHIP 建连.
 */
static esp_err_t ensure_ws_frag_buffer(void)
{
    if (s_frag_buf != NULL)
    {
        return ESP_OK;
    }

    s_frag_buf = (char *)heap_caps_malloc(WS_FRAG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_frag_buf == NULL)
    {
        s_frag_buf = (char *)malloc(WS_FRAG_BUF_SIZE);
    }
    if (s_frag_buf == NULL)
    {
        ESP_LOGE(TAG, "创建 WS 分片缓冲失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "WS 分片缓冲已就绪: size=%u psram=%u",
             (unsigned)WS_FRAG_BUF_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000LL);
}

static const char *ws_event_name(int32_t event_id)
{
    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        return "connected";
    case WEBSOCKET_EVENT_DISCONNECTED:
        return "disconnected";
    case WEBSOCKET_EVENT_DATA:
        return "data";
    case WEBSOCKET_EVENT_ERROR:
        return "error";
    case WEBSOCKET_EVENT_CLOSED:
        return "closed";
    default:
        return "unknown";
    }
}

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

static bool str_same(const char *a, const char *b)
{
    if (a == NULL)
    {
        a = "";
    }
    if (b == NULL)
    {
        b = "";
    }
    return strcmp(a, b) == 0;
}

static uint32_t next_msg_id(void)
{
    uint32_t id;
    portENTER_CRITICAL(&s_msg_id_lock);
    id = ++s_msg_id;
    portEXIT_CRITICAL(&s_msg_id_lock);
    return id;
}

static bool auth_cache_ready(void)
{
    bool ready;
    portENTER_CRITICAL(&s_auth_lock);
    ready = (s_cached_auth.openid[0] != '\0' && s_cached_auth.model_id[0] != '\0');
    portEXIT_CRITICAL(&s_auth_lock);
    return ready;
}

static void remember_auth_user(const char *openid, const char *model_id, const char *wx_app_id, const char *source)
{
    if (openid == NULL || openid[0] == '\0')
    {
        return;
    }

    portENTER_CRITICAL(&s_auth_lock);
    bool same_openid = str_same(s_cached_auth.openid, openid);
    const char *next_model_id = model_id && model_id[0] ? model_id : (same_openid ? s_cached_auth.model_id : "");
    const char *next_app_id = wx_app_id && wx_app_id[0] ? wx_app_id : (same_openid ? s_cached_auth.app_id : "");
    bool changed = !same_openid ||
                   !str_same(s_cached_auth.model_id, next_model_id) ||
                   !str_same(s_cached_auth.app_id, next_app_id);
    copy_str(s_cached_auth.openid, sizeof(s_cached_auth.openid), openid);
    copy_str(s_cached_auth.model_id, sizeof(s_cached_auth.model_id), next_model_id);
    copy_str(s_cached_auth.app_id, sizeof(s_cached_auth.app_id), next_app_id);
    portEXIT_CRITICAL(&s_auth_lock);

    if (changed)
    {
        WX_VOIP_TRACEI(TAG,
                       "已缓存微信呼叫用户: 来源=%s openid_len=%u model_id_len=%u wx_app_id=%s",
                       source ? source : "unknown",
                       (unsigned)strlen(openid),
                       (unsigned)strlen(next_model_id),
                       next_app_id[0] ? next_app_id : "(空)");
    }
    else
    {
        WX_VOIP_TRACEI(TAG, "微信授权缓存未变化: 来源=%s", source ? source : "unknown");
    }
}

static void parse_auth_list_payload(cJSON *payload, const char *type)
{
    cJSON *list = cJSON_GetObjectItemCaseSensitive(payload, "list");
    if (!cJSON_IsArray(list))
    {
        ESP_LOGW(TAG, "微信授权列表格式错误: %s", type);
        return;
    }

    int count = cJSON_GetArraySize(list);
    WX_VOIP_TRACEI(TAG, "收到微信授权列表: %s count=%d", type, count);
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, list)
    {
        const char *openid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "wx_open_id"));
        const char *model_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "wx_model_id"));
        const char *wx_app_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "wx_app_id"));
        remember_auth_user(openid, model_id, wx_app_id, type);
    }
}

static bool build_device_call_url(char *out, size_t out_size)
{
    const char *uri = TIRTC_WX_VOIP_WS_URI;
    const char *scheme = NULL;
    size_t ws_scheme_len = 0;

    if (strncmp(uri, "ws://", 5) == 0)
    {
        scheme = "http://";
        ws_scheme_len = 5;
    }
    else if (strncmp(uri, "wss://", 6) == 0)
    {
        scheme = "https://";
        ws_scheme_len = 6;
    }
    else
    {
        return false;
    }

    const char *suffix = strstr(uri + ws_scheme_len, "/device/ws");
    if (suffix == NULL)
    {
        return false;
    }

    int n = snprintf(out,
                     out_size,
                     "%s%.*s/device/call",
                     scheme,
                     (int)(suffix - (uri + ws_scheme_len)),
                     uri + ws_scheme_len);
    return n > 0 && n < (int)out_size;
}

static esp_err_t send_ws_text(const char *json)
{
    if (json == NULL || s_client == NULL || !s_connected)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int len = (int)strlen(json);
    WX_VOIP_TRACEI(TAG,
                   "WS 发送: len=%d connected=%d text=%.160s",
                   len,
                   s_connected ? 1 : 0,
                   json);
    int sent = esp_websocket_client_send_text(s_client, json, len, pdMS_TO_TICKS(5000));
    xSemaphoreGive(s_send_mutex);

    WX_VOIP_TRACEI(TAG, "WS 发送完成: sent=%d expected=%d", sent, len);
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_ws_type(const char *type)
{
    char buf[128];
    uint32_t id = next_msg_id();
    int n = snprintf(buf, sizeof(buf),
                     "{\"v\":1,\"type\":\"%s\",\"id\":\"D%lu\",\"ts\":%lld}",
                     type,
                     (unsigned long)id,
                     (long long)now_ms());
    if (n > 0 && n < (int)sizeof(buf))
    {
        return send_ws_text(buf);
    }
    return ESP_ERR_INVALID_SIZE;
}

static void send_ack(const char *msg_id)
{
    if (msg_id == NULL || msg_id[0] == '\0')
    {
        return;
    }

    char buf[160];
    int n = snprintf(buf,
                     sizeof(buf),
                     "{\"v\":1,\"type\":\"ack\",\"id\":\"%s\",\"ts\":%lld}",
                     msg_id,
                     (long long)now_ms());
    if (n > 0 && n < (int)sizeof(buf))
    {
        (void)send_ws_text(buf);
    }
}

static void request_auth_cache(const char *reason)
{
    esp_err_t ret = send_ws_type("query_auth");
    if (ret == ESP_OK)
    {
        WX_VOIP_TRACEI(TAG, "正在同步微信授权缓存: %s", reason ? reason : "手动触发");
    }
    else
    {
        ESP_LOGW(TAG, "微信授权缓存同步请求未发送: %s", esp_err_to_name(ret));
    }
}

static const char *active_call_state_name(active_call_state_t state)
{
    switch (state)
    {
    case ACTIVE_CALL_IDLE:
        return "空闲";
    case ACTIVE_CALL_REQUESTING:
        return "请求中";
    case ACTIVE_CALL_WAIT_JOIN:
        return "等待微信接听";
    default:
        return "未知";
    }
}

static void active_call_set_idle_locked(void)
{
    s_active_call_state = ACTIVE_CALL_IDLE;
    s_active_call_deadline_us = 0;
    s_active_call_stage[0] = '\0';
}

static void active_call_set_stage(const char *stage)
{
    portENTER_CRITICAL(&s_active_call_lock);
    copy_str(s_active_call_stage, sizeof(s_active_call_stage), stage);
    portEXIT_CRITICAL(&s_active_call_lock);
    WX_VOIP_TRACEI(TAG, "主动呼叫阶段: %s", stage ? stage : "无");
}

static void active_call_reset_if_expired(const char *reason)
{
    active_call_state_t old_state = ACTIVE_CALL_IDLE;
    bool expired = false;
    int64_t now_us = esp_timer_get_time();
    char stage[sizeof(s_active_call_stage)] = {0};

    portENTER_CRITICAL(&s_active_call_lock);
    if (s_active_call_state != ACTIVE_CALL_IDLE &&
        s_active_call_deadline_us > 0 &&
        now_us >= s_active_call_deadline_us)
    {
        old_state = s_active_call_state;
        copy_str(stage, sizeof(stage), s_active_call_stage);
        ++s_active_call_seq;
        active_call_set_idle_locked();
        expired = true;
    }
    portEXIT_CRITICAL(&s_active_call_lock);

    if (expired)
    {
        ESP_LOGW(TAG,
                 "主动呼叫%s超时,已回到空闲: %s stage=%s",
                 active_call_state_name(old_state),
                 reason ? reason : "超时回收",
                 stage[0] ? stage : "无");
    }
}

static esp_err_t active_call_begin(uint32_t *seq)
{
    active_call_reset_if_expired("发起前检查");
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_active_call_lock);
    if (s_active_call_state != ACTIVE_CALL_IDLE)
    {
        active_call_state_t busy_state = s_active_call_state;
        portEXIT_CRITICAL(&s_active_call_lock);
        ESP_LOGW(TAG, "主动呼叫正在%s,请稍后", active_call_state_name(busy_state));
        return ESP_ERR_INVALID_STATE;
    }

    s_active_call_state = ACTIVE_CALL_REQUESTING;
    s_active_call_deadline_us = now_us + (int64_t)ACTIVE_CALL_REQUEST_GUARD_MS * 1000;
    copy_str(s_active_call_stage, sizeof(s_active_call_stage), "等待任务启动");
    *seq = ++s_active_call_seq;
    portEXIT_CRITICAL(&s_active_call_lock);
    WX_VOIP_TRACEI(TAG, "主动呼叫进入请求状态: seq=%lu", (unsigned long)*seq);
    return ESP_OK;
}

static void active_call_abort(uint32_t seq)
{
    bool aborted = false;

    portENTER_CRITICAL(&s_active_call_lock);
    if (seq == s_active_call_seq)
    {
        active_call_set_idle_locked();
        aborted = true;
    }
    portEXIT_CRITICAL(&s_active_call_lock);

    if (aborted)
    {
        WX_VOIP_TRACEI(TAG, "主动呼叫中止: seq=%lu", (unsigned long)seq);
    }
}

static bool active_call_is_current_request(uint32_t seq)
{
    bool current = false;

    portENTER_CRITICAL(&s_active_call_lock);
    current = (seq == s_active_call_seq && s_active_call_state == ACTIVE_CALL_REQUESTING);
    portEXIT_CRITICAL(&s_active_call_lock);

    return current;
}

static void active_call_finish(uint32_t seq, esp_err_t result)
{
    bool stale = false;
    bool submitted = false;
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_active_call_lock);
    if (seq != s_active_call_seq || s_active_call_state != ACTIVE_CALL_REQUESTING)
    {
        stale = true;
    }
    else if (result == ESP_OK)
    {
        s_active_call_state = ACTIVE_CALL_WAIT_JOIN;
        s_active_call_deadline_us = now_us + (int64_t)ACTIVE_CALL_JOIN_WAIT_MS * 1000;
        copy_str(s_active_call_stage, sizeof(s_active_call_stage), "等待微信接听");
        submitted = true;
    }
    else
    {
        active_call_set_idle_locked();
    }
    portEXIT_CRITICAL(&s_active_call_lock);

    if (stale)
    {
        ESP_LOGD(TAG, "忽略过期主动呼叫任务结果: %s", esp_err_to_name(result));
        return;
    }
    if (submitted)
    {
        ESP_LOGI(TAG, "微信呼叫请求已提交,等待微信接听,接听后设备自动入会");
        WX_VOIP_TRACEI(TAG,
                       "主动呼叫等待入会: seq=%lu wait=%ums",
                       (unsigned long)seq,
                       (unsigned)ACTIVE_CALL_JOIN_WAIT_MS);
    }
    else if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "主动呼叫请求未提交,已回到空闲: %s", esp_err_to_name(result));
    }
}

static bool take_active_call_join_pending(void)
{
    bool pending = false;
    uint32_t seq = 0;

    active_call_reset_if_expired("入会前检查");

    portENTER_CRITICAL(&s_active_call_lock);
    if (s_active_call_state == ACTIVE_CALL_WAIT_JOIN)
    {
        pending = true;
        seq = s_active_call_seq;
        active_call_set_idle_locked();
    }
    portEXIT_CRITICAL(&s_active_call_lock);

    if (pending)
    {
        WX_VOIP_TRACEI(TAG, "主动呼叫入会消息命中: seq=%lu", (unsigned long)seq);
    }
    return pending;
}

static void active_call_read_status(active_call_state_t *state,
                                    char *stage,
                                    size_t stage_size,
                                    int64_t *deadline_left_ms)
{
    int64_t deadline_us = 0;
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_active_call_lock);
    if (state != NULL)
    {
        *state = s_active_call_state;
    }
    if (stage != NULL && stage_size > 0)
    {
        copy_str(stage, stage_size, s_active_call_stage);
    }
    deadline_us = s_active_call_deadline_us;
    portEXIT_CRITICAL(&s_active_call_lock);

    if (deadline_left_ms != NULL)
    {
        *deadline_left_ms = deadline_us > now_us ? (deadline_us - now_us) / 1000 : 0;
    }
}

static void auth_sync_task(void *arg)
{
    (void)arg;

    for (int retry = 0; retry < AUTH_SYNC_RETRY_COUNT; ++retry)
    {
        if (!s_connected)
        {
            break;
        }
        request_auth_cache(retry == 0 ? "业务连接建立" : "缓存未就绪, 再次查询");
        vTaskDelay(pdMS_TO_TICKS(AUTH_SYNC_RETRY_MS));

        if (auth_cache_ready())
        {
            ESP_LOGI(TAG, "微信授权缓存已就绪");
            break;
        }
    }

    if (s_connected && !auth_cache_ready())
    {
        ESP_LOGW(TAG, "微信授权缓存未获取到, 请确认微信用户已绑定并且业务服务器支持 query_auth");
    }

    s_auth_sync_task = NULL;
    vTaskDelete(NULL);
}

static void start_auth_sync(void)
{
    if (s_auth_sync_task != NULL)
    {
        return;
    }

    BaseType_t ret = xTaskCreateWithCaps(auth_sync_task,
                                         "wx_auth_sync",
                                         AUTH_SYNC_TASK_STACK,
                                         NULL,
                                         AUTH_SYNC_TASK_PRIORITY,
                                         &s_auth_sync_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_auth_sync_task = NULL;
        ESP_LOGW(TAG, "创建微信授权同步任务失败, 直接查询一次");
        request_auth_cache("任务创建失败后的兜底查询");
    }
}

static void send_device_media(void)
{
    char buf[384];
    int n = snprintf(buf, sizeof(buf),
                     "{\"v\":1,\"type\":\"device_media\","
                     "\"id\":\"D%lu\",\"ts\":%lld,"
                     "\"payload\":{"
                     "\"screen_width\":1,"
                     "\"screen_height\":1,"
                     "\"audio_rate\":%u,"
                     "\"audio_channels\":%u,"
                     "\"video_mt\":\"none\","
                     "\"no_video\":true,"
                     "\"calling_timeout_sec\":%u"
                     "}}",
                     (unsigned long)next_msg_id(),
                     (long long)now_ms(),
                     DEVICE_AUDIO_RATE,
                     DEVICE_AUDIO_CHANNELS,
                     DEVICE_CALLING_TIMEOUT_SEC);
    if (n > 0 && n < (int)sizeof(buf))
    {
        (void)send_ws_text(buf);
        WX_VOIP_TRACEI(TAG, "已上报示例媒体能力: no_video=true audio=%uHz/%uch",
                       DEVICE_AUDIO_RATE,
                       DEVICE_AUDIO_CHANNELS);
    }
}

static void load_active_call_target(wx_auth_user_t *target)
{
    if (target == NULL)
    {
        return;
    }

    memset(target, 0, sizeof(*target));

    portENTER_CRITICAL(&s_auth_lock);
    *target = s_cached_auth;
    portEXIT_CRITICAL(&s_auth_lock);
}

static esp_err_t parse_http_url(const char *url,
                                char *host,
                                size_t host_size,
                                char *port,
                                size_t port_size,
                                const char **path)
{
    const char prefix[] = "http://";
    const size_t prefix_len = sizeof(prefix) - 1U;

    if (url == NULL || host == NULL || port == NULL || path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, prefix, prefix_len) != 0)
    {
        ESP_LOGE(TAG, "主动呼叫暂只支持 http 地址: %s", url);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *host_begin = url + prefix_len;
    const char *path_begin = strchr(host_begin, '/');
    if (path_begin == NULL || path_begin == host_begin)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *port_begin = memchr(host_begin, ':', (size_t)(path_begin - host_begin));
    size_t host_len = port_begin ? (size_t)(port_begin - host_begin) : (size_t)(path_begin - host_begin);
    if (host_len == 0 || host_len >= host_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(host, host_begin, host_len);
    host[host_len] = '\0';

    if (port_begin != NULL)
    {
        size_t port_len = (size_t)(path_begin - port_begin - 1);
        if (port_len == 0 || port_len >= port_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(port, port_begin + 1, port_len);
        port[port_len] = '\0';
    }
    else
    {
        strlcpy(port, "80", port_size);
    }

    *path = path_begin;
    return ESP_OK;
}

static esp_err_t ensure_dns_semaphore(void)
{
    if (s_dns_done_sem != NULL)
    {
        return ESP_OK;
    }

    s_dns_done_sem = xSemaphoreCreateBinary();
    if (s_dns_done_sem == NULL)
    {
        ESP_LOGE(TAG, "创建 DNS 等待信号量失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool parse_tcp_port(const char *port, uint16_t *out)
{
    if (port == NULL || out == NULL || port[0] == '\0')
    {
        return false;
    }

    char *end = NULL;
    long value = strtol(port, &end, 10);
    if (end == port || *end != '\0' || value <= 0 || value > 65535)
    {
        return false;
    }

    *out = (uint16_t)value;
    return true;
}

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    uint32_t seq = (uint32_t)(uintptr_t)arg;
    bool should_notify = false;

    portENTER_CRITICAL(&s_dns_lock);
    if (s_dns_lookup.waiting && s_dns_lookup.seq == seq)
    {
        s_dns_lookup.done = true;
        s_dns_lookup.ok = (ipaddr != NULL && IP_IS_V4(ipaddr));
        if (s_dns_lookup.ok)
        {
            s_dns_lookup.addr = *ipaddr;
        }
        should_notify = true;
    }
    portEXIT_CRITICAL(&s_dns_lock);

    if (should_notify && s_dns_done_sem != NULL)
    {
        xSemaphoreGive(s_dns_done_sem);
    }
}

static void dns_cache_save(const char *host, const ip_addr_t *addr)
{
    if (host == NULL || addr == NULL || !IP_IS_V4(addr))
    {
        return;
    }

    copy_str(s_dns_cache_host, sizeof(s_dns_cache_host), host);
    s_dns_cache_addr = *addr;
    s_dns_cache_valid = true;
}

static bool dns_cache_load(const char *host, ip_addr_t *addr)
{
    if (host == NULL || addr == NULL)
    {
        return false;
    }
    if (!s_dns_cache_valid || strcmp(s_dns_cache_host, host) != 0)
    {
        return false;
    }

    *addr = s_dns_cache_addr;
    return true;
}

static void fill_sockaddr_ipv4(const ip_addr_t *ipaddr, uint16_t port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    out->sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(ipaddr));
}

static esp_err_t resolve_host_ipv4(const char *host,
                                   const char *port,
                                   struct sockaddr_in *out,
                                   bool *from_cache)
{
    if (host == NULL || port == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (from_cache != NULL)
    {
        *from_cache = false;
    }

    uint16_t port_num = 0;
    if (!parse_tcp_port(port, &port_num))
    {
        ESP_LOGW(TAG, "主动呼叫端口格式错误: %s", port);
        return ESP_ERR_INVALID_ARG;
    }

    ip_addr_t addr;
    if (dns_cache_load(host, &addr))
    {
        fill_sockaddr_ipv4(&addr, port_num, out);
        if (from_cache != NULL)
        {
            *from_cache = true;
        }
        WX_VOIP_TRACEI(TAG, "主动呼叫 HTTP 阶段: 复用 DNS 缓存 host=%s", host);
        return ESP_OK;
    }

    esp_err_t sem_ret = ensure_dns_semaphore();
    if (sem_ret != ESP_OK)
    {
        return sem_ret;
    }

    while (xSemaphoreTake(s_dns_done_sem, 0) == pdTRUE)
    {
    }

    uint32_t seq = 0;
    portENTER_CRITICAL(&s_dns_lock);
    seq = ++s_dns_lookup_seq;
    memset(&s_dns_lookup, 0, sizeof(s_dns_lookup));
    s_dns_lookup.seq = seq;
    s_dns_lookup.waiting = true;
    portEXIT_CRITICAL(&s_dns_lock);

    active_call_set_stage("解析呼叫服务器地址");
    WX_VOIP_TRACEI(TAG, "主动呼叫 HTTP 阶段: 解析服务器 host=%s", host);
    err_t dns_ret = dns_gethostbyname(host, &addr, dns_found_cb, (void *)(uintptr_t)seq);
    if (dns_ret == ERR_OK)
    {
        portENTER_CRITICAL(&s_dns_lock);
        if (s_dns_lookup.seq == seq)
        {
            s_dns_lookup.waiting = false;
        }
        portEXIT_CRITICAL(&s_dns_lock);

        if (!IP_IS_V4(&addr))
        {
            ESP_LOGW(TAG, "主动呼叫 DNS 返回非 IPv4 地址: host=%s", host);
            return ESP_FAIL;
        }

        dns_cache_save(host, &addr);
        fill_sockaddr_ipv4(&addr, port_num, out);
        WX_VOIP_TRACEI(TAG, "主动呼叫 HTTP 阶段: DNS 已完成");
        return ESP_OK;
    }
    if (dns_ret != ERR_INPROGRESS)
    {
        portENTER_CRITICAL(&s_dns_lock);
        if (s_dns_lookup.seq == seq)
        {
            s_dns_lookup.waiting = false;
        }
        portEXIT_CRITICAL(&s_dns_lock);
        ESP_LOGW(TAG, "主动呼叫 DNS 解析启动失败: host=%s ret=%d", host, dns_ret);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_dns_done_sem, pdMS_TO_TICKS(ACTIVE_CALL_DNS_TIMEOUT_MS)) != pdTRUE)
    {
        portENTER_CRITICAL(&s_dns_lock);
        if (s_dns_lookup.seq == seq)
        {
            s_dns_lookup.waiting = false;
        }
        portEXIT_CRITICAL(&s_dns_lock);
        ESP_LOGW(TAG, "主动呼叫 DNS 解析超时: host=%s timeout=%ums", host, (unsigned)ACTIVE_CALL_DNS_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    bool ok = false;
    portENTER_CRITICAL(&s_dns_lock);
    if (s_dns_lookup.seq == seq && s_dns_lookup.done && s_dns_lookup.ok)
    {
        addr = s_dns_lookup.addr;
        ok = true;
    }
    s_dns_lookup.waiting = false;
    portEXIT_CRITICAL(&s_dns_lock);

    if (!ok)
    {
        ESP_LOGW(TAG, "主动呼叫 DNS 解析失败: host=%s", host);
        return ESP_FAIL;
    }

    dns_cache_save(host, &addr);
    fill_sockaddr_ipv4(&addr, port_num, out);
    WX_VOIP_TRACEI(TAG, "主动呼叫 HTTP 阶段: DNS 已完成");
    return ESP_OK;
}

static int socket_deadline_left_ms(int64_t deadline_us)
{
    int64_t left_us = deadline_us - esp_timer_get_time();
    if (left_us <= 0)
    {
        return 0;
    }

    int64_t left_ms = (left_us + 999) / 1000;
    return left_ms > INT32_MAX ? INT32_MAX : (int)left_ms;
}

static esp_err_t socket_wait_ready(int sock, bool write_ready, int64_t deadline_us)
{
    while (true)
    {
        int timeout_ms = socket_deadline_left_ms(deadline_us);
        if (timeout_ms <= 0)
        {
            return ESP_ERR_TIMEOUT;
        }

        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        if (write_ready)
        {
            FD_SET(sock, &write_set);
        }
        else
        {
            FD_SET(sock, &read_set);
        }

        struct timeval timeout = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        int ret = select(sock + 1,
                         write_ready ? NULL : &read_set,
                         write_ready ? &write_set : NULL,
                         NULL,
                         &timeout);
        if (ret > 0)
        {
            return ESP_OK;
        }
        if (ret == 0)
        {
            return ESP_ERR_TIMEOUT;
        }
        if (errno != EINTR)
        {
            return ESP_FAIL;
        }
    }
}

static esp_err_t socket_send_all(int sock, const char *data, size_t len, int64_t deadline_us)
{
    size_t sent = 0;

    while (sent < len)
    {
        int ret = send(sock, data + sent, len - sent, 0);
        if (ret > 0)
        {
            sent += (size_t)ret;
            continue;
        }
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            esp_err_t wait_ret = socket_wait_ready(sock, true, deadline_us);
            if (wait_ret != ESP_OK)
            {
                return wait_ret;
            }
            continue;
        }

        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t socket_connect_with_timeout(int sock,
                                             const struct sockaddr *addr,
                                             socklen_t addrlen,
                                             int timeout_ms)
{
    if (timeout_ms <= 0)
    {
        return ESP_ERR_TIMEOUT;
    }

    int64_t start_us = esp_timer_get_time();
    int64_t deadline_us = start_us + (int64_t)timeout_ms * 1000;

    active_call_set_stage("准备 TCP 连接");
    WX_VOIP_TRACEI(TAG, "主动呼叫 TCP: 设置非阻塞 socket");
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
    {
        ESP_LOGW(TAG, "主动呼叫 TCP: 读取 socket 标志失败 errno=%d", errno);
        return ESP_FAIL;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        ESP_LOGW(TAG, "主动呼叫 TCP: 设置非阻塞失败 errno=%d", errno);
        return ESP_FAIL;
    }

    active_call_set_stage("发起 TCP 连接");
    WX_VOIP_TRACEI(TAG, "主动呼叫 TCP: connect begin timeout=%dms", timeout_ms);
    int ret = connect(sock, addr, addrlen);
    int saved_errno = errno;
    WX_VOIP_TRACEI(TAG, "主动呼叫 TCP: connect ret=%d errno=%d", ret, saved_errno);
    if (ret == 0)
    {
        WX_VOIP_TRACEI(TAG, "主动呼叫 TCP: connect 立即成功");
        return ESP_OK;
    }
    if (saved_errno != EINPROGRESS)
    {
        errno = saved_errno;
        return ESP_FAIL;
    }

    active_call_set_stage("等待 TCP 连接完成");
    while (true)
    {
        int left_ms = socket_deadline_left_ms(deadline_us);
        if (left_ms <= 0)
        {
            WX_VOIP_TRACEW(TAG,
                           "主动呼叫 TCP: select 等待超时 cost=%lldms",
                           (long long)((esp_timer_get_time() - start_us) / 1000));
            return ESP_ERR_TIMEOUT;
        }

        int wait_ms = left_ms > 250 ? 250 : left_ms;
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);

        struct timeval timeout = {
            .tv_sec = wait_ms / 1000,
            .tv_usec = (wait_ms % 1000) * 1000,
        };
        ret = select(sock + 1, NULL, &write_set, NULL, &timeout);
        saved_errno = errno;
        if (ret > 0)
        {
            WX_VOIP_TRACEI(TAG,
                           "主动呼叫 TCP: select ready cost=%lldms",
                           (long long)((esp_timer_get_time() - start_us) / 1000));
            break;
        }
        if (ret == 0)
        {
            continue;
        }
        if (saved_errno == EINTR)
        {
            continue;
        }

        errno = saved_errno;
        ESP_LOGW(TAG, "主动呼叫 TCP: select 失败 errno=%d", errno);
        return ESP_FAIL;
    }

    active_call_set_stage("检查 TCP 连接结果");
    int sock_err = 0;
    socklen_t sock_err_len = sizeof(sock_err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len) < 0)
    {
        ESP_LOGW(TAG, "主动呼叫 TCP: getsockopt 失败 errno=%d", errno);
        return ESP_FAIL;
    }
    WX_VOIP_TRACEI(TAG, "主动呼叫 TCP: SO_ERROR=%d", sock_err);

    if (sock_err != 0)
    {
        errno = sock_err;
        return ESP_FAIL;
    }

    WX_VOIP_TRACEI(TAG,
                   "主动呼叫 TCP: 连接成功 cost=%lldms",
                   (long long)((esp_timer_get_time() - start_us) / 1000));
    return ESP_OK;
}

static esp_err_t http_post_json_raw(const char *url,
                                    const char *body,
                                    int body_len,
                                    char *response_buf,
                                    size_t response_size,
                                    int *status_code)
{
    /*
     * /device/call 是一个很短的 HTTP POST. 这里直接用 socket 串行发送,
     * 让每次请求的 DNS、连接、发送和响应阶段都可观察, 也便于及时释放资源.
     */
    char host[128];
    char port[ACTIVE_CALL_HTTP_PORT_SIZE];
    const char *path = NULL;
    esp_err_t ret = parse_http_url(url, host, sizeof(host), port, sizeof(port), &path);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (response_buf == NULL || response_size == 0 || status_code == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    response_buf[0] = '\0';
    *status_code = 0;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)ACTIVE_CALL_HTTP_HARD_TIMEOUT_MS * 1000;

    struct sockaddr_in server_addr;
    bool dns_from_cache = false;
    ret = resolve_host_ipv4(host, port, &server_addr, &dns_from_cache);
    if (ret != ESP_OK)
    {
        return ret;
    }

    int connect_timeout_ms = socket_deadline_left_ms(deadline_us);
    if (connect_timeout_ms <= 0)
    {
        return ESP_ERR_TIMEOUT;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGW(TAG, "主动呼叫 socket 创建失败: errno=%d", errno);
        return ESP_FAIL;
    }

    struct timeval timeout = {
        .tv_sec = ACTIVE_CALL_HTTP_TIMEOUT_MS / 1000,
        .tv_usec = (ACTIVE_CALL_HTTP_TIMEOUT_MS % 1000) * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    active_call_set_stage("连接呼叫服务器");
    WX_VOIP_TRACEI(TAG,
                   "主动呼叫 HTTP 阶段: 连接服务器 dns_cache=%d",
                   dns_from_cache ? 1 : 0);
    if (socket_connect_with_timeout(sock,
                                    (const struct sockaddr *)&server_addr,
                                    sizeof(server_addr),
                                    connect_timeout_ms) != ESP_OK)
    {
        ESP_LOGW(TAG, "主动呼叫 TCP 连接失败: errno=%d", errno);
        if (dns_from_cache)
        {
            s_dns_cache_valid = false;
        }
        close(sock);
        return ESP_FAIL;
    }

    char head[ACTIVE_CALL_HTTP_HEAD_SIZE];
    int head_len = snprintf(head,
                            sizeof(head),
                            "POST %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Content-Type: application/json\r\n"
                            "Accept: application/json\r\n"
                            "Connection: close\r\n"
                            "Content-Length: %d\r\n"
                            "\r\n",
                            path,
                            host,
                            body_len);
    if (head_len <= 0 || head_len >= (int)sizeof(head))
    {
        close(sock);
        return ESP_ERR_INVALID_SIZE;
    }

    active_call_set_stage("发送呼叫请求");
    WX_VOIP_TRACEI(TAG, "主动呼叫 HTTP 阶段: 发送请求");
    ret = socket_send_all(sock, head, (size_t)head_len, deadline_us);
    if (ret == ESP_OK)
    {
        ret = socket_send_all(sock, body, (size_t)body_len, deadline_us);
    }
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "主动呼叫 HTTP 发送失败: %s errno=%d", esp_err_to_name(ret), errno);
        close(sock);
        return ret;
    }

    active_call_set_stage("读取呼叫响应");
    WX_VOIP_TRACEI(TAG, "主动呼叫 HTTP 阶段: 等待响应");
    size_t used = 0;
    while (used + 1U < response_size)
    {
        ret = socket_wait_ready(sock, false, deadline_us);
        if (ret == ESP_ERR_TIMEOUT && used > 0)
        {
            break;
        }
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG,
                     "主动呼叫 HTTP 等待响应失败: %s errno=%d used=%u",
                     esp_err_to_name(ret),
                     errno,
                     (unsigned)used);
            close(sock);
            return ret;
        }

        int got = recv(sock, response_buf + used, response_size - used - 1U, 0);
        if (got > 0)
        {
            used += (size_t)got;
            response_buf[used] = '\0';
            continue;
        }
        if (got == 0)
        {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            continue;
        }
        ESP_LOGW(TAG, "主动呼叫 HTTP 接收失败: errno=%d", errno);
        close(sock);
        return ESP_FAIL;
    }

    close(sock);
    if (used == 0)
    {
        ESP_LOGW(TAG, "主动呼叫 HTTP 响应为空");
        return ESP_ERR_TIMEOUT;
    }
    WX_VOIP_TRACEI(TAG, "主动呼叫 HTTP 阶段: 响应已读取 bytes=%u", (unsigned)used);

    int parsed_status = 0;
    if (sscanf(response_buf, "HTTP/%*s %d", &parsed_status) != 1)
    {
        ESP_LOGW(TAG, "主动呼叫 HTTP 响应头异常: %.120s", response_buf);
        return ESP_FAIL;
    }
    *status_code = parsed_status;

    char *body_start = strstr(response_buf, "\r\n\r\n");
    if (body_start == NULL)
    {
        response_buf[0] = '\0';
        return ESP_OK;
    }

    body_start += 4;
    char *json_begin = strchr(body_start, '{');
    char *json_end = strrchr(body_start, '}');
    if (json_begin != NULL && json_end != NULL && json_end >= json_begin)
    {
        size_t json_len = (size_t)(json_end - json_begin + 1);
        memmove(response_buf, json_begin, json_len);
        response_buf[json_len] = '\0';
    }
    else
    {
        memmove(response_buf, body_start, strlen(body_start) + 1U);
    }

    return ESP_OK;
}

static esp_err_t do_active_call_http(uint32_t seq)
{
    wx_auth_user_t target;
    active_call_set_stage("读取授权缓存");
    if (!active_call_is_current_request(seq))
    {
        return ESP_ERR_INVALID_STATE;
    }
    load_active_call_target(&target);

    if (target.openid[0] == '\0')
    {
        ESP_LOGW(TAG, "无法主动呼叫: 缺少微信 openid, 请先确认业务连接已收到授权缓存");
        return ESP_ERR_INVALID_STATE;
    }
    if (target.model_id[0] == '\0')
    {
        ESP_LOGW(TAG, "无法主动呼叫: 缺少微信 model_id, 请先确认业务连接已收到授权缓存");
        return ESP_ERR_INVALID_STATE;
    }

    WX_VOIP_TRACEI(TAG,
                   "主动呼叫使用微信授权缓存: openid=%s model_id=%s wx_app_id=%s version_type=%d",
                   target.openid,
                   target.model_id,
                   target.app_id[0] ? target.app_id : "(未携带)",
                   TIRTC_WX_VOIP_ACTIVE_CALL_VERSION_TYPE);

    char url[512];
    active_call_set_stage("生成请求地址");
    if (!active_call_is_current_request(seq))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!build_device_call_url(url, sizeof(url)))
    {
        ESP_LOGE(TAG, "主动呼叫地址生成失败");
        return ESP_ERR_INVALID_ARG;
    }
    WX_VOIP_TRACEI(TAG, "主动呼叫地址已生成: %s", url);

    char wx_query[160];
    snprintf(wx_query, sizeof(wx_query), "device_id=%s&source=%s", TIRTC_DEVICE_ID, ACTIVE_CALL_SOURCE);

    char body[768];
    int body_len = 0;
    active_call_set_stage("生成请求内容");
    if (!active_call_is_current_request(seq))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (target.app_id[0] != '\0')
    {
        body_len = snprintf(body,
                            sizeof(body),
                            "{\"device_id\":\"%s\","
                            "\"wx_user_openid\":\"%s\","
                            "\"wx_model_id\":\"%s\","
                            "\"wx_room_type\":\"%s\","
                            "\"wx_version_type\":%d,"
                            "\"wx_caller_camera_status\":0,"
                            "\"wx_listener_camera_status\":0,"
                            "\"wx_listener_name\":\"\","
                            "\"wx_query\":\"%s\","
                            "\"wx_app_id\":\"%s\"}",
                            TIRTC_DEVICE_ID,
                            target.openid,
                            target.model_id,
                            ACTIVE_CALL_ROOM_TYPE,
                            TIRTC_WX_VOIP_ACTIVE_CALL_VERSION_TYPE,
                            wx_query,
                            target.app_id);
    }
    else
    {
        body_len = snprintf(body,
                            sizeof(body),
                            "{\"device_id\":\"%s\","
                            "\"wx_user_openid\":\"%s\","
                            "\"wx_model_id\":\"%s\","
                            "\"wx_room_type\":\"%s\","
                            "\"wx_version_type\":%d,"
                            "\"wx_caller_camera_status\":0,"
                            "\"wx_listener_camera_status\":0,"
                            "\"wx_listener_name\":\"\","
                            "\"wx_query\":\"%s\"}",
                            TIRTC_DEVICE_ID,
                            target.openid,
                            target.model_id,
                            ACTIVE_CALL_ROOM_TYPE,
                            TIRTC_WX_VOIP_ACTIVE_CALL_VERSION_TYPE,
                            wx_query);
    }
    if (body_len <= 0 || body_len >= (int)sizeof(body))
    {
        ESP_LOGE(TAG, "主动呼叫请求内容过长");
        return ESP_ERR_INVALID_SIZE;
    }

    WX_VOIP_TRACEI(TAG,
                   "主动呼叫请求已准备: body=%u heap=%u internal=%u",
                   (unsigned)body_len,
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    char response_buf[ACTIVE_CALL_HTTP_RESP_SIZE] = {0};
    int status = 0;

    ESP_LOGI(TAG, "正在请求微信呼叫: POST %s", url);
    active_call_set_stage("执行 HTTP POST");
    if (!active_call_is_current_request(seq))
    {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t start_us = esp_timer_get_time();
    esp_err_t ret = http_post_json_raw(url, body, body_len, response_buf, sizeof(response_buf), &status);
    int64_t cost_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG,
             "微信呼叫请求返回: ret=%s status=%d cost=%lldms",
             esp_err_to_name(ret),
             status,
             (long long)cost_ms);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "主动呼叫请求失败: %s cost=%lldms,可再次按 BOOT 重试",
                 esp_err_to_name(ret),
                 (long long)cost_ms);
        return ret;
    }
    if (status != 200)
    {
        ESP_LOGW(TAG, "主动呼叫被服务器拒绝: HTTP %d body=%.160s", status, response_buf);
        return ESP_FAIL;
    }

    active_call_set_stage("解析服务器响应");
    cJSON *reply = cJSON_Parse(response_buf);
    if (reply == NULL)
    {
        ESP_LOGW(TAG, "主动呼叫响应格式错误: %.160s", response_buf);
        return ESP_FAIL;
    }
    cJSON *code = cJSON_GetObjectItemCaseSensitive(reply, "code");
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(reply, "msg");
    int code_value = cJSON_IsNumber(code) ? code->valueint : -1;
    char msg_value[96];
    copy_str(msg_value, sizeof(msg_value), cJSON_GetStringValue(msg));
    cJSON_Delete(reply);

    if (code_value != 0)
    {
        ESP_LOGW(TAG, "主动呼叫未通过: code=%d msg=%s", code_value, msg_value);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* 主动呼叫 HTTP 在固定 worker 中串行执行,避免每轮呼叫反复申请内部 RAM 栈. */
static void active_call_task(void *arg)
{
    (void)arg;

    while (true)
    {
        uint32_t seq = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &seq, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        WX_VOIP_TRACEI(TAG, "主动呼叫任务开始: seq=%lu", (unsigned long)seq);
        esp_err_t ret = do_active_call_http(seq);
        WX_VOIP_TRACEI(TAG, "主动呼叫任务结束: seq=%lu ret=%s", (unsigned long)seq, esp_err_to_name(ret));
        active_call_finish(seq, ret);
    }
}

static esp_err_t ensure_active_call_worker(void)
{
    if (s_active_call_worker_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(active_call_task,
                                         "wx_voip_call",
                                         ACTIVE_CALL_TASK_STACK,
                                         NULL,
                                         ACTIVE_CALL_TASK_PRIORITY,
                                         &s_active_call_worker_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_active_call_worker_task = NULL;
        ESP_LOGE(TAG, "创建主动呼叫任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t wechat_voip_request_call(void)
{
    if (s_client == NULL || !s_connected)
    {
        ESP_LOGW(TAG, "无法主动呼叫: 业务连接未建立");
        return ESP_ERR_INVALID_STATE;
    }

    if (!wechat_voip_session_ready_for_next_call(true))
    {
        ESP_LOGW(TAG, "无法主动呼叫: 通话资源未就绪");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t seq = 0;
    esp_err_t begin_ret = active_call_begin(&seq);
    if (begin_ret != ESP_OK)
    {
        return begin_ret;
    }

    esp_err_t worker_ret = ensure_active_call_worker();
    if (worker_ret != ESP_OK)
    {
        active_call_abort(seq);
        return worker_ret;
    }

    if (xTaskNotify(s_active_call_worker_task, seq, eSetValueWithOverwrite) != pdPASS)
    {
        active_call_abort(seq);
        ESP_LOGE(TAG, "唤醒主动呼叫任务失败");
        return ESP_FAIL;
    }

    WX_VOIP_TRACEI(TAG, "主动呼叫任务已唤醒: seq=%lu", (unsigned long)seq);
    return ESP_OK;
}

bool wechat_voip_request_call_busy(void)
{
    active_call_reset_if_expired("状态检查");

    portENTER_CRITICAL(&s_active_call_lock);
    active_call_state_t state = s_active_call_state;
    bool busy = (s_active_call_state != ACTIVE_CALL_IDLE);
    char stage[sizeof(s_active_call_stage)] = {0};
    copy_str(stage, sizeof(stage), s_active_call_stage);
    portEXIT_CRITICAL(&s_active_call_lock);

    if (busy)
    {
        WX_VOIP_TRACEI(TAG,
                       "主动呼叫当前状态: %s stage=%s",
                       active_call_state_name(state),
                       stage[0] ? stage : "无");
    }
    return busy;
}

static bool cancel_active_call_wait(const char *reason, bool warn_remote_still_ringing)
{
    active_call_state_t old_state = ACTIVE_CALL_IDLE;

    portENTER_CRITICAL(&s_active_call_lock);
    if (s_active_call_state != ACTIVE_CALL_IDLE)
    {
        old_state = s_active_call_state;
        ++s_active_call_seq;
        active_call_set_idle_locked();
    }
    portEXIT_CRITICAL(&s_active_call_lock);

    if (old_state == ACTIVE_CALL_IDLE)
    {
        return false;
    }

    ESP_LOGI(TAG,
             "已取消本地主动呼叫等待: %s,%s",
             active_call_state_name(old_state),
             reason ? reason : "本地取消");
    if (warn_remote_still_ringing)
    {
        ESP_LOGW(TAG, "未接听前没有 TiRTC 连接, 微信侧响铃需要业务服务器取消");
    }
    return true;
}

void wechat_voip_cancel_pending_call(void)
{
    (void)cancel_active_call_wait("本地取消", true);
}

void wechat_voip_ws_maintenance(void)
{
    active_call_reset_if_expired("后台维护");
}

static bool msg_type_is(const char *type, const char *name1, const char *name2)
{
    return type != NULL &&
           (strcmp(type, name1) == 0 ||
            (name2 != NULL && strcmp(type, name2) == 0));
}

static const char *json_string_any(cJSON *root, const char *name1, const char *name2)
{
    if (root == NULL || name1 == NULL)
    {
        return NULL;
    }

    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name1));
    if ((value == NULL || value[0] == '\0') && name2 != NULL)
    {
        value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name2));
    }
    return value;
}

static void log_business_event(const char *type, const char *msg_id, const char *room_id)
{
    if (msg_type_is(type, "ping", NULL) || msg_type_is(type, "pong", NULL))
    {
        return;
    }

    active_call_state_t state = ACTIVE_CALL_IDLE;
    char stage[sizeof(s_active_call_stage)] = {0};
    int64_t deadline_left_ms = 0;
    active_call_read_status(&state, stage, sizeof(stage), &deadline_left_ms);

    WX_VOIP_TRACEI(TAG,
                   "业务事件: type=%s id=%s room=%s active=%s stage=%s wait=%lldms",
                   type ? type : "(空)",
                   msg_id ? msg_id : "(空)",
                   room_id && room_id[0] ? room_id : "(空)",
                   active_call_state_name(state),
                   stage[0] ? stage : "无",
                   (long long)deadline_left_ms);
}

static void handle_envelope(const char *json, int len)
{
    WX_VOIP_TRACEI(TAG, "WS 收到完整业务消息: len=%d", len);

    WX_VOIP_TRACEI(TAG, "WS 业务消息开始解析");
    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
    {
        ESP_LOGW(TAG, "业务消息解析失败");
        return;
    }
    WX_VOIP_TRACEI(TAG, "WS 业务消息解析完成");

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "type"));
    const char *msg_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id"));
    if (type == NULL)
    {
        ESP_LOGW(TAG, "业务消息缺少 type");
        cJSON_Delete(root);
        return;
    }

    cJSON *payload_for_log = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const char *room_for_log = json_string_any(payload_for_log, "wx_room_id", "wxa_room_id");
    if (room_for_log == NULL || room_for_log[0] == '\0')
    {
        room_for_log = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "room_id"));
    }
    log_business_event(type, msg_id, room_for_log);

    if (msg_type_is(type, "ping", NULL))
    {
        WX_VOIP_TRACEI(TAG, "处理业务 ping");
        send_ws_type("pong");
        cJSON_Delete(root);
        return;
    }

    if (msg_type_is(type, "pong", NULL))
    {
        WX_VOIP_TRACEI(TAG, "处理业务 pong");
        cJSON_Delete(root);
        return;
    }

    if (msg_type_is(type, "device_media_ack", NULL))
    {
        request_auth_cache("业务服务器确认媒体能力");
        WX_VOIP_TRACEI(TAG, "业务服务器已确认媒体能力");
        cJSON_Delete(root);
        return;
    }

    if (msg_type_is(type, "device_call_voip_ack", "device_call_ack"))
    {
        WX_VOIP_TRACEI(TAG, "业务服务器已确认主动呼叫请求");
        cJSON_Delete(root);
        return;
    }

    if (msg_type_is(type, "auth_list", "auth_update"))
    {
        WX_VOIP_TRACEI(TAG, "处理授权缓存消息: type=%s", type);
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        parse_auth_list_payload(payload, type);
        send_ack(msg_id);
        cJSON_Delete(root);
        return;
    }

    if (msg_type_is(type, "wx_user_bind", "wxa_user_bind"))
    {
        WX_VOIP_TRACEI(TAG, "处理微信绑定消息: type=%s", type);
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        const char *openid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "wx_user_openid"));
        const char *model_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "wx_model_id"));
        const char *wx_app_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "wx_app_id"));
        remember_auth_user(openid, model_id, wx_app_id, type);
        send_ack(msg_id);
        ESP_LOGI(TAG, "收到微信用户绑定消息");
        cJSON_Delete(root);
        return;
    }

    if (msg_type_is(type, "wx_join_voip_room", "wxa_join_voip_room"))
    {
        send_ack(msg_id);
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        if (cJSON_IsObject(payload))
        {
            bool auto_answer = take_active_call_join_pending();
            const char *peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "peer_id"));
            const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "token"));
            WX_VOIP_TRACEI(TAG,
                           "处理入会消息: auto=%d peer_id_len=%u token_len=%u",
                           auto_answer ? 1 : 0,
                           peer_id ? (unsigned)strlen(peer_id) : 0,
                           token ? (unsigned)strlen(token) : 0);
            ESP_LOGI(TAG, "%s", auto_answer ? "收到主动呼叫入会消息,自动建立通话" : "收到微信来电消息");
            esp_err_t ret = wechat_voip_session_handle_join_room(payload, auto_answer);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "处理微信来电失败: %s", esp_err_to_name(ret));
            }
            else
            {
                WX_VOIP_TRACEI(TAG, "入会消息已交给会话模块");
            }
        }
        else
        {
            ESP_LOGE(TAG, "微信来电消息内容为空");
        }
        cJSON_Delete(root);
        return;
    }

    if (msg_type_is(type, "wx_user_cancel", "wxa_user_cancel"))
    {
        WX_VOIP_TRACEI(TAG, "处理微信取消消息: type=%s", type);
        send_ack(msg_id);
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        const char *room_id = json_string_any(payload, "wx_room_id", "wxa_room_id");
        if (room_id == NULL || room_id[0] == '\0')
        {
            room_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "room_id"));
        }
        bool matched = wechat_voip_session_cancel_by_room(room_id);
        if (!matched)
        {
            if (cancel_active_call_wait("微信侧取消", false))
            {
                ESP_LOGI(TAG, "微信侧已取消当前主动呼叫等待");
            }
            else
            {
                ESP_LOGW(TAG,
                         "微信取消事件未命中当前通话,已忽略: room=%s",
                         room_id && room_id[0] ? room_id : "(空)");
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "error") == 0)
    {
        char *raw = cJSON_PrintUnformatted(root);
        ESP_LOGW(TAG, "业务服务器返回错误: %.240s", raw ? raw : "");
        free(raw);
        cJSON_Delete(root);
        return;
    }

    char *raw = cJSON_PrintUnformatted(root);
    ESP_LOGW(TAG, "收到未处理业务消息 type=%s raw=%.240s", type, raw ? raw : "");
    WX_VOIP_TRACEW(TAG, "未处理业务消息: type=%s len=%d", type, len);
    free(raw);
    cJSON_Delete(root);
}

static void ws_msg_task(void *arg)
{
    (void)arg;

    while (true)
    {
        ws_msg_item_t item = {0};
        if (xQueueReceive(s_ws_msg_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (item.json != NULL && item.len > 0)
        {
            handle_envelope(item.json, item.len);
        }
        free(item.json);
    }
}

static esp_err_t ensure_ws_msg_worker(void)
{
    if (s_ws_msg_queue == NULL)
    {
        s_ws_msg_queue = xQueueCreate(WS_MSG_QUEUE_LEN, sizeof(ws_msg_item_t));
        if (s_ws_msg_queue == NULL)
        {
            ESP_LOGE(TAG, "创建 WS 业务消息队列失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_ws_msg_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(ws_msg_task,
                                         "wx_ws_msg",
                                         WS_MSG_TASK_STACK,
                                         NULL,
                                         WS_MSG_TASK_PRIORITY,
                                         &s_ws_msg_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_ws_msg_task = NULL;
        ESP_LOGE(TAG, "创建 WS 业务消息任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t enqueue_ws_message(const char *json, int len)
{
    if (json == NULL || len <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_ws_msg_worker();
    if (ret != ESP_OK)
    {
        return ret;
    }

    char *copy = heap_caps_malloc((size_t)len + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL)
    {
        copy = malloc((size_t)len + 1U);
    }
    if (copy == NULL)
    {
        ESP_LOGW(TAG, "WS 业务消息缓存申请失败: len=%d", len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';

    ws_msg_item_t item = {
        .json = copy,
        .len = len,
    };
    if (xQueueSend(s_ws_msg_queue, &item, 0) != pdPASS)
    {
        free(copy);
        ESP_LOGW(TAG, "WS 业务消息队列已满,丢弃消息 len=%d", len);
        return ESP_ERR_TIMEOUT;
    }

    WX_VOIP_TRACEI(TAG,
                   "WS 业务消息已投递: len=%d queue=%u",
                   len,
                   (unsigned)uxQueueMessagesWaiting(s_ws_msg_queue));
    return ESP_OK;
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    WX_VOIP_TRACEI(TAG,
                   "WS 事件: %s(%ld) data_len=%d payload_len=%d op=0x%02x fin=%d",
                   ws_event_name(event_id),
                   (long)event_id,
                   data ? data->data_len : 0,
                   data ? data->payload_len : 0,
                   data ? data->op_code : 0,
                   data ? data->fin : 0);

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "业务连接已建立, 开始同步微信授权缓存");
        s_connected = true;
        s_frag_len = 0;
        send_device_media();
        start_auth_sync();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "业务连接已断开");
        s_connected = false;
        s_frag_len = 0;
        wechat_voip_cancel_pending_call();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == NULL)
        {
            WX_VOIP_TRACEW(TAG, "WS 数据事件缺少内容");
            break;
        }
        /*
         * esp_websocket_client 会把 ping/pong/close 这类控制帧也走到 DATA 事件.
         * 控制帧允许没有 payload,先按 opcode 过滤,避免把正常 pong 误判成空业务包.
         */
        if (data->op_code != 0x01 && data->op_code != 0x00)
        {
            WX_VOIP_TRACEI(TAG, "忽略非文本 WS 数据: op=0x%02x len=%d", data->op_code, data->data_len);
            break;
        }
        if (data->data_ptr == NULL || data->data_len <= 0)
        {
            WX_VOIP_TRACEW(TAG, "WS 文本数据为空");
            break;
        }
        if (s_frag_buf == NULL)
        {
            ESP_LOGW(TAG, "WS 分片缓冲未就绪");
            s_frag_len = 0;
            break;
        }
        if (data->payload_len >= WS_FRAG_BUF_SIZE)
        {
            ESP_LOGW(TAG, "业务消息过大: %d bytes", data->payload_len);
            s_frag_len = 0;
            break;
        }
        if (s_frag_len + data->data_len < WS_FRAG_BUF_SIZE)
        {
            memcpy(s_frag_buf + s_frag_len, data->data_ptr, data->data_len);
            s_frag_len += data->data_len;
            WX_VOIP_TRACEI(TAG,
                           "WS 数据分片累计: frag=%d payload=%d fin=%d",
                           s_frag_len,
                           data->payload_len,
                           data->fin);
        }
        if (data->fin)
        {
            s_frag_buf[s_frag_len] = '\0';
            (void)enqueue_ws_message(s_frag_buf, s_frag_len);
            s_frag_len = 0;
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "业务连接异常");
        s_connected = false;
        wechat_voip_cancel_pending_call();
        break;

    default:
        break;
    }
}

static void websocket_task(void *arg)
{
    (void)arg;
    int64_t last_ping_ms = now_ms();

    int n = snprintf(s_uri, sizeof(s_uri), "%s?device_id=%s",
                     TIRTC_WX_VOIP_WS_URI,
                     TIRTC_DEVICE_ID);
    if (n <= 0 || n >= (int)sizeof(s_uri))
    {
        ESP_LOGE(TAG, "业务连接地址过长");
        vTaskDelete(NULL);
        return;
    }

    esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .reconnect_timeout_ms = WS_RECONNECT_TIMEOUT_MS,
        .network_timeout_ms = 10000,
        .task_stack = 12288,
        .buffer_size = WS_FRAG_BUF_SIZE,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "业务连接初始化失败");
        vTaskDelete(NULL);
        return;
    }

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    esp_err_t ret = esp_websocket_client_start(s_client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "业务连接启动失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "业务连接正在启动");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t current_ms = now_ms();
        if (s_connected && current_ms - last_ping_ms >= WS_PING_INTERVAL_MS)
        {
            (void)send_ws_type("ping");
            last_ping_ms = current_ms;
        }
    }
}

esp_err_t wechat_voip_ws_start(void)
{
    if (s_client != NULL)
    {
        return ESP_OK;
    }

    esp_err_t worker_ret = ensure_ws_frag_buffer();
    if (worker_ret != ESP_OK)
    {
        return worker_ret;
    }

    worker_ret = ensure_active_call_worker();
    if (worker_ret != ESP_OK)
    {
        return worker_ret;
    }

    s_send_mutex = xSemaphoreCreateMutex();
    if (s_send_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    worker_ret = ensure_ws_msg_worker();
    if (worker_ret != ESP_OK)
    {
        return worker_ret;
    }

    BaseType_t ret = xTaskCreateWithCaps(websocket_task,
                                         "wx_voip_ws",
                                         WS_TASK_STACK,
                                         NULL,
                                         WS_TASK_PRIORITY,
                                         NULL,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ret == pdPASS ? ESP_OK : ESP_FAIL;
}

bool wechat_voip_ws_is_connected(void)
{
    return s_client != NULL && s_connected;
}
