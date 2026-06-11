/*
 * 微信 VoIP 会话示例.
 *
 * 业务服务器下发入会通知后,本文件保存本次 peer_id/token,
 * 再由 BOOT 接听或主动呼叫自动接听触发 TiRtcWhipConnect().
 * WHIP 建连后等待 CALL_CONNECTED, 确认通话进入媒体阶段后再发音频.
 */
#include "wechat_voip_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "tiRTC.h"
#include "tirtc_config.h"
#include "tirtc_voip_cmdw.h"
#include "wechat_voip_media.h"
#include "wechat_voip_trace.h"

static const char *TAG = "wx_voip";

enum
{
    VOIP_ANSWER_TASK_STACK = 49152,
    VOIP_ANSWER_TASK_PRIORITY = 5,
    VOIP_BOOT_ANSWER_DELAY_MS = 500,
    VOIP_ACTIVE_ANSWER_DELAY_MS = 1200,
    VOIP_RING_TIMEOUT_MS = 35000,
    VOIP_CONNECT_TIMEOUT_MS = 20000,
    VOIP_CONNECTED_WAIT_TIMEOUT_MS = 15000,
    VOIP_CLOSE_WAIT_TIMEOUT_MS = 1500,
    VOIP_RECALL_GUARD_MS = 1200,
    VOIP_STATUS_WARN_INTERVAL_MS = 2000,
    VOIP_MEDIA_STOP_WAIT_MS = 300,
    VOIP_WORK_QUEUE_LEN = 8,
    VOIP_WORK_TASK_STACK = 12288,
    VOIP_WORK_TASK_PRIORITY = 5,
    VOIP_DISCONNECT_DELAY_MS = 120,
};

typedef enum
{
    VOIP_STATE_IDLE,
    VOIP_STATE_RINGING,
    VOIP_STATE_CONNECTING,
    VOIP_STATE_AWAITING_CONNECTED,
    VOIP_STATE_IN_CALL,
    VOIP_STATE_CLOSING,
} voip_state_t;

typedef struct
{
    voip_state_t state;
    tirtc_conn_t hconn;
    char peer_id[2048];
    char token[1024];
    char wx_app_id[64];
    char wx_model_id[64];
    char wx_session_token[256];
    char wx_room_id[128];
    char wx_payload[256];
    int64_t deadline_us;
} voip_session_t;

typedef struct
{
    char wx_app_id[64];
    char wx_model_id[64];
    char wx_session_token[256];
    char wx_room_id[128];
    char wx_payload[256];
} voip_reject_info_t;

typedef enum
{
    VOIP_WORK_DISCONNECT,
    VOIP_WORK_HANGUP,
    VOIP_WORK_REJECT,
} voip_work_type_t;

typedef struct
{
    voip_work_type_t type;
    tirtc_conn_t hconn;
    tirtc_voip_hangup_reason_t reason;
    bool only_if_current;
    voip_reject_info_t reject;
} voip_work_item_t;

typedef struct
{
    voip_state_t state;
    tirtc_conn_t hconn;
    bool answer_pending;
    TaskHandle_t answer_task;
    bool work_busy;
    voip_work_type_t work_type;
    uint32_t work_queue_len;
    bool media_running;
    int64_t deadline_left_ms;
    int64_t last_close_ago_ms;
} voip_status_t;

static SemaphoreHandle_t s_mutex;
static voip_session_t s_session;
static TaskHandle_t s_answer_worker_task;
static QueueHandle_t s_work_queue;
static TaskHandle_t s_work_task;
static int64_t s_last_close_us;
static bool s_answer_pending;
static char s_answer_source[32];
static uint32_t s_answer_delay_ms;
static portMUX_TYPE s_work_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_work_busy;
static voip_work_type_t s_work_type;
static int64_t s_last_status_warn_us;

static void voip_work_task(void *arg);
static esp_err_t ensure_work_worker(void);
static esp_err_t enqueue_work(const voip_work_item_t *item);
static esp_err_t send_reject_info(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason);

static void ensure_init(void)
{
    if (s_mutex != NULL)
    {
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
    s_session.state = VOIP_STATE_IDLE;
    (void)ensure_work_worker();
}

static void lock_session(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock_session(void)
{
    xSemaphoreGive(s_mutex);
}

static bool lock_session_wait(const char *where, uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        return true;
    }

    ESP_LOGW(TAG, "等待会话锁超时: %s", where ? where : "未知位置");
    return false;
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

static const char *state_name(voip_state_t state)
{
    switch (state)
    {
    case VOIP_STATE_IDLE:
        return "空闲";
    case VOIP_STATE_RINGING:
        return "振铃";
    case VOIP_STATE_CONNECTING:
        return "连接中";
    case VOIP_STATE_AWAITING_CONNECTED:
        return "等待接通";
    case VOIP_STATE_IN_CALL:
        return "通话中";
    case VOIP_STATE_CLOSING:
        return "关闭中";
    default:
        return "未知";
    }
}

static const char *work_type_name(voip_work_type_t type)
{
    switch (type)
    {
    case VOIP_WORK_DISCONNECT:
        return "断开";
    case VOIP_WORK_HANGUP:
        return "挂断";
    case VOIP_WORK_REJECT:
        return "拒接";
    default:
        return "未知";
    }
}

static void clear_session_locked(void)
{
    WX_VOIP_TRACEI(TAG, "清空会话: old_state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = VOIP_STATE_IDLE;
    s_answer_pending = false;
    s_answer_source[0] = '\0';
    s_answer_delay_ms = 0;
}

static void finish_session_locked(void)
{
    WX_VOIP_TRACEI(TAG, "结束会话: state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    clear_session_locked();
    s_last_close_us = esp_timer_get_time();
}

static void set_deadline_locked(uint32_t timeout_ms)
{
    s_session.deadline_us = timeout_ms == 0 ? 0 : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
}

static void begin_close_locked(void)
{
    WX_VOIP_TRACEI(TAG, "进入关闭状态: old_state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    s_session.state = VOIP_STATE_CLOSING;
    s_answer_pending = false;
    s_answer_source[0] = '\0';
    s_answer_delay_ms = 0;
    set_deadline_locked(VOIP_CLOSE_WAIT_TIMEOUT_MS);
}

static bool connection_is_current(tirtc_conn_t hconn)
{
    if (hconn == NULL)
    {
        return false;
    }

    lock_session();
    bool current = (s_session.hconn == hconn && s_session.state != VOIP_STATE_IDLE);
    unlock_session();
    return current;
}

static void disconnect_later(tirtc_conn_t hconn, bool only_if_current)
{
    if (hconn == NULL)
    {
        return;
    }

    WX_VOIP_TRACEI(TAG,
                   "投递断开任务: hconn=%p only_if_current=%d",
                   hconn,
                   only_if_current ? 1 : 0);

    voip_work_item_t item = {
        .type = VOIP_WORK_DISCONNECT,
        .hconn = hconn,
        .only_if_current = only_if_current,
    };
    if (enqueue_work(&item) != ESP_OK)
    {
        ESP_LOGW(TAG, "断开任务投递失败,等待下一次维护");
    }
}

static bool voip_cmd_is(uint32_t cmdw, uint32_t expected)
{
    return cmdw == expected ||
           (cmdw & 0xffffU) == expected ||
           (cmdw & 0x7fffU) == expected;
}

static void extract_query_param(const char *url, const char *key, char *out, size_t out_size)
{
    if (url == NULL || key == NULL || out == NULL || out_size == 0)
    {
        return;
    }
    out[0] = '\0';

    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *p = strstr(url, search);
    if (p == NULL)
    {
        return;
    }
    p += strlen(search);

    size_t i = 0;
    while (*p != '\0' && *p != '&' && i < out_size - 1)
    {
        out[i++] = *p++;
    }
    out[i] = '\0';
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

static void on_reject_response(const char *body, void *user_data)
{
    (void)user_data;
    if (body != NULL && body[0] != '\0')
    {
        WX_VOIP_TRACEI(TAG, "拒接响应: %.120s", body);
    }
}

static esp_err_t send_reject(const voip_session_t *sess, tirtc_voip_hangup_reason_t reason)
{
    if (sess == NULL || sess->wx_room_id[0] == '\0')
    {
        ESP_LOGW(TAG, "拒接请求缺少房间信息");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "拒接请求内容创建失败");
        return ESP_ERR_NO_MEM;
    }
    /* 平台拒接接口使用 wx_* 字段; wxa_* 作为旧示例服务端兼容字段保留. */
    cJSON_AddStringToObject(root, "wx_app_id", sess->wx_app_id);
    cJSON_AddStringToObject(root, "wx_model_id", sess->wx_model_id);
    cJSON_AddStringToObject(root, "wx_session_token", sess->wx_session_token);
    cJSON_AddStringToObject(root, "wx_room_id", sess->wx_room_id);
    cJSON_AddStringToObject(root, "wx_payload", sess->wx_payload);
    cJSON_AddStringToObject(root, "wxa_app_id", sess->wx_app_id);
    cJSON_AddStringToObject(root, "wxa_model_id", sess->wx_model_id);
    cJSON_AddStringToObject(root, "wxa_session_token", sess->wx_session_token);
    cJSON_AddStringToObject(root, "wxa_room_id", sess->wx_room_id);
    cJSON_AddStringToObject(root, "wxa_payload", sess->wx_payload);
    cJSON_AddNumberToObject(root, "hangup_reason", (int)reason);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        ESP_LOGE(TAG, "拒接请求内容序列化失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "发送微信拒接: reason=%d", (int)reason);
    WX_VOIP_TRACEI(TAG,
                   "调用 TiRtcServiceRequest 拒接: room=%s app_id=%s model_id=%s session_token_len=%u payload_len=%u",
                   sess->wx_room_id,
                   sess->wx_app_id[0] ? sess->wx_app_id : "(空)",
                   sess->wx_model_id[0] ? sess->wx_model_id : "(空)",
                   (unsigned)strlen(sess->wx_session_token),
                   (unsigned)strlen(sess->wx_payload));
    int rc = TiRtcServiceRequest("/v1/wxvoip/reject", body, NULL, on_reject_response, NULL);
    free(body);
    if (rc != 0)
    {
        ESP_LOGW(TAG, "通知 TiRTC 拒接失败: %d %s", rc, TiRtcGetErrorStr(rc));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void fill_reject_info(voip_reject_info_t *info, const voip_session_t *sess)
{
    if (info == NULL)
    {
        return;
    }

    memset(info, 0, sizeof(*info));
    if (sess == NULL)
    {
        return;
    }

    copy_str(info->wx_app_id, sizeof(info->wx_app_id), sess->wx_app_id);
    copy_str(info->wx_model_id, sizeof(info->wx_model_id), sess->wx_model_id);
    copy_str(info->wx_session_token, sizeof(info->wx_session_token), sess->wx_session_token);
    copy_str(info->wx_room_id, sizeof(info->wx_room_id), sess->wx_room_id);
    copy_str(info->wx_payload, sizeof(info->wx_payload), sess->wx_payload);
}

static esp_err_t send_reject_info(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    voip_session_t sess = {0};
    copy_str(sess.wx_app_id, sizeof(sess.wx_app_id), info->wx_app_id);
    copy_str(sess.wx_model_id, sizeof(sess.wx_model_id), info->wx_model_id);
    copy_str(sess.wx_session_token, sizeof(sess.wx_session_token), info->wx_session_token);
    copy_str(sess.wx_room_id, sizeof(sess.wx_room_id), info->wx_room_id);
    copy_str(sess.wx_payload, sizeof(sess.wx_payload), info->wx_payload);
    return send_reject(&sess, reason);
}

static void set_work_busy(bool busy, voip_work_type_t type)
{
    portENTER_CRITICAL(&s_work_state_lock);
    s_work_busy = busy;
    s_work_type = type;
    portEXIT_CRITICAL(&s_work_state_lock);
    WX_VOIP_TRACEI(TAG, "工作队列状态: busy=%d type=%s", busy ? 1 : 0, work_type_name(type));
}

static void voip_work_task(void *arg)
{
    (void)arg;

    /*
     * TiRTC 的挂断、拒接、断开都可能触发网络和 SDK 内部收尾.
     * 统一放到固定 worker 中串行执行,避免按键任务、维护任务和 SDK 回调被拖住.
     */
    while (true)
    {
        voip_work_item_t item;
        if (xQueueReceive(s_work_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        set_work_busy(true, item.type);
        WX_VOIP_TRACEI(TAG,
                       "开始处理工作项: type=%s hconn=%p reason=%d only_if_current=%d",
                       work_type_name(item.type),
                       item.hconn,
                       (int)item.reason,
                       item.only_if_current ? 1 : 0);
        switch (item.type)
        {
        case VOIP_WORK_HANGUP:
            if (item.hconn != NULL)
            {
                char body[32];
                int n = snprintf(body, sizeof(body), "{\"reason\":%d}", (int)item.reason);
                if (connection_is_current(item.hconn))
                {
                    (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                    if (n > 0 && n < (int)sizeof(body))
                    {
                        WX_VOIP_TRACEI(TAG,
                                       "发送 TiRTC 挂断命令: hconn=%p reason=%d",
                                       item.hconn,
                                       (int)item.reason);
                        (void)TiRtcSendCommand(item.hconn, TIRTC_VOIP_HANGUP, body, (uint32_t)strlen(body));
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(VOIP_DISCONNECT_DELAY_MS));
                if (connection_is_current(item.hconn))
                {
                    WX_VOIP_TRACEI(TAG, "调用 TiRtcDisconnect: hconn=%p", item.hconn);
                    (void)TiRtcDisconnect(item.hconn);
                }
                ESP_LOGI(TAG, "微信通话挂断流程已提交");
            }
            break;

        case VOIP_WORK_DISCONNECT:
            if (item.hconn != NULL)
            {
                if (!item.only_if_current || connection_is_current(item.hconn))
                {
                    (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                    vTaskDelay(pdMS_TO_TICKS(VOIP_DISCONNECT_DELAY_MS));
                    if (!item.only_if_current || connection_is_current(item.hconn))
                    {
                        ESP_LOGI(TAG, "释放微信通话连接");
                        WX_VOIP_TRACEI(TAG, "调用 TiRtcDisconnect: hconn=%p", item.hconn);
                        (void)TiRtcDisconnect(item.hconn);
                    }
                }
            }
            break;

        case VOIP_WORK_REJECT:
            if (send_reject_info(&item.reject, item.reason) == ESP_OK)
            {
                ESP_LOGI(TAG, "%s", item.reason == TIRTC_VOIP_HANGUP_REASON_REJECT ? "已拒接微信来电" : "已取消微信通话");
            }
            break;
        }
        WX_VOIP_TRACEI(TAG, "工作项处理完成: type=%s", work_type_name(item.type));
        set_work_busy(false, item.type);
    }
}

static esp_err_t ensure_work_worker(void)
{
    if (s_work_queue == NULL)
    {
        s_work_queue = xQueueCreate(VOIP_WORK_QUEUE_LEN, sizeof(voip_work_item_t));
        if (s_work_queue == NULL)
        {
            ESP_LOGE(TAG, "创建微信 VoIP 工作队列失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_work_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(voip_work_task,
                                         "wx_voip_work",
                                         VOIP_WORK_TASK_STACK,
                                         NULL,
                                         VOIP_WORK_TASK_PRIORITY,
                                         &s_work_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_work_task = NULL;
        ESP_LOGE(TAG, "创建微信 VoIP 工作任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t enqueue_work(const voip_work_item_t *item)
{
    if (item == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_work_worker();
    if (ret != ESP_OK)
    {
        return ret;
    }
    WX_VOIP_TRACEI(TAG,
                   "投递工作项: type=%s hconn=%p reason=%d queue_before=%u",
                   work_type_name(item->type),
                   item->hconn,
                   (int)item->reason,
                   s_work_queue ? (unsigned)uxQueueMessagesWaiting(s_work_queue) : 0);
    return xQueueSend(s_work_queue, item, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t reject_later(const voip_session_t *sess, tirtc_voip_hangup_reason_t reason)
{
    voip_work_item_t item = {
        .type = VOIP_WORK_REJECT,
        .reason = reason,
    };
    fill_reject_info(&item.reject, sess);

    esp_err_t ret = enqueue_work(&item);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "拒接任务投递失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void on_whip_connect(int error, tirtc_conn_t hconn, void *user_data)
{
    (void)user_data;
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "WHIP 回调: error=%d hconn=%p",
                   error,
                   hconn);

    if (error != 0)
    {
        ESP_LOGE(TAG, "通话连接失败: %d %s", error, TiRtcGetErrorStr(error));
        lock_session();
        voip_session_t failed = s_session;
        finish_session_locked();
        unlock_session();
        (void)reject_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        return;
    }

    lock_session();
    if (s_session.state == VOIP_STATE_CONNECTING)
    {
        WX_VOIP_TRACEI(TAG,
                       "WHIP 连接进入等待接通: old_state=%s hconn=%p",
                       state_name(s_session.state),
                       hconn);
        s_session.hconn = hconn;
        s_session.state = VOIP_STATE_AWAITING_CONNECTED;
        set_deadline_locked(VOIP_CONNECTED_WAIT_TIMEOUT_MS);
        unlock_session();
        ESP_LOGI(TAG, "通话连接已建立,等待接通确认");
        return;
    }
    if ((s_session.state == VOIP_STATE_AWAITING_CONNECTED || s_session.state == VOIP_STATE_IN_CALL) &&
        s_session.hconn == hconn)
    {
        voip_state_t state = s_session.state;
        unlock_session();
        ESP_LOGD(TAG, "连接回调重复到达,当前状态=%s", state_name(state));
        return;
    }
    unlock_session();

    ESP_LOGW(TAG, "连接回调到达时会话已结束,释放连接");
    disconnect_later(hconn, false);
}

static esp_err_t answer_current_call(const char *source)
{
    lock_session();
    if (s_session.state != VOIP_STATE_RINGING)
    {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    s_session.state = VOIP_STATE_CONNECTING;
    set_deadline_locked(VOIP_CONNECT_TIMEOUT_MS);
    voip_session_t snapshot = s_session;
    unlock_session();

    ESP_LOGI(TAG, "%s,正在建立通话", source);
    WX_VOIP_TRACEI(TAG,
                   "准备调用 TiRtcWhipConnect: source=%s room=%s peer_id_len=%u token_len=%u",
                   source ? source : "(空)",
                   snapshot.wx_room_id[0] ? snapshot.wx_room_id : "(空)",
                   (unsigned)strlen(snapshot.peer_id),
                   (unsigned)strlen(snapshot.token));

    int64_t start_us = esp_timer_get_time();
    WX_VOIP_TRACEI(TAG, "调用 TiRtcWhipConnect");
    int rc = TiRtcWhipConnect(snapshot.peer_id, snapshot.token, on_whip_connect, NULL);
    int64_t cost_ms = (esp_timer_get_time() - start_us) / 1000;
    WX_VOIP_TRACEI(TAG, "TiRtcWhipConnect 返回: rc=%d cost=%lldms", rc, (long long)cost_ms);
    if (rc == 0)
    {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "提交通话连接失败: %d %s", rc, TiRtcGetErrorStr(rc));
    lock_session();
    voip_session_t failed = s_session;
    finish_session_locked();
    unlock_session();
    (void)reject_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
    return ESP_FAIL;
}

/* 每次入会创建一次接听任务,任务结束即删除,避免跨轮通话残留任务状态。 */
static void answer_task(void *arg)
{
    (void)arg;
    ensure_init();

    char source[sizeof(s_answer_source)];
    uint32_t delay_ms = 0;
    lock_session();
    copy_str(source, sizeof(source), s_answer_source[0] ? s_answer_source : "BOOT 键接听");
    delay_ms = s_answer_delay_ms;
    unlock_session();

    /*
     * 入会消息代表可以准备建连,但微信侧房间状态仍可能在切换中.
     * 主动呼叫自动入会稍等更久一点,用于避开“刚接听就立刻 WHIP”的时序抖动.
     */
    if (delay_ms > 0)
    {
        WX_VOIP_TRACEI(TAG, "接听任务等待房间稳定: source=%s delay=%ums", source, (unsigned)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    WX_VOIP_TRACEI(TAG, "接听任务开始: source=%s", source);
    esp_err_t ret = answer_current_call(source);

    lock_session();
    s_answer_pending = false;
    s_answer_source[0] = '\0';
    s_answer_delay_ms = 0;
    s_answer_worker_task = NULL;
    unlock_session();

    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "当前没有待接来电");
    }

    WX_VOIP_TRACEI(TAG, "接听任务结束: ret=%s", esp_err_to_name(ret));
    vTaskDelete(NULL);
}

static esp_err_t start_answer_worker(void)
{
    if (s_answer_worker_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreateWithCaps(answer_task,
                                         "wx_voip_answer",
                                         VOIP_ANSWER_TASK_STACK,
                                         NULL,
                                         VOIP_ANSWER_TASK_PRIORITY,
                                         &s_answer_worker_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_answer_worker_task = NULL;
        ESP_LOGE(TAG, "创建接听任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t start_answer_task(const char *source, uint32_t delay_ms)
{
    if (!lock_session_wait("启动接听任务", 1000))
    {
        return ESP_ERR_TIMEOUT;
    }
    bool ringing = (s_session.state == VOIP_STATE_RINGING);
    bool pending = s_answer_pending;
    voip_state_t state = s_session.state;
    if (ringing && !pending)
    {
        s_answer_pending = true;
        copy_str(s_answer_source, sizeof(s_answer_source), source ? source : "BOOT 键接听");
        s_answer_delay_ms = delay_ms;
    }
    unlock_session();
    if (!ringing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    WX_VOIP_TRACEI(TAG,
                   "请求启动接听任务: source=%s state=%s pending=%d delay=%ums",
                   source ? source : "BOOT 键接听",
                   state_name(state),
                   pending ? 1 : 0,
                   (unsigned)delay_ms);

    ESP_LOGD(TAG, "准备启动接听任务: %s", source ? source : "BOOT 键接听");

    if (pending)
    {
        ESP_LOGW(TAG, "正在接听,请勿重复触发");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "%s请求已收到", source ? source : "BOOT 键接听");

    esp_err_t worker_ret = start_answer_worker();
    if (worker_ret != ESP_OK)
    {
        if (lock_session_wait("回滚接听任务状态", 1000))
        {
            s_answer_pending = false;
            s_answer_source[0] = '\0';
            s_answer_delay_ms = 0;
            unlock_session();
        }
        return worker_ret;
    }

    ESP_LOGD(TAG, "接听任务已创建");
    return ESP_OK;
}

esp_err_t wechat_voip_session_handle_join_room(cJSON *root, bool auto_answer)
{
    ensure_init();

    if (!cJSON_IsObject(root))
    {
        ESP_LOGE(TAG, "来电消息为空");
        return ESP_ERR_INVALID_ARG;
    }

    WX_VOIP_TRACEI(TAG, "开始处理入会消息: auto=%d", auto_answer ? 1 : 0);
    WX_VOIP_TRACEI(TAG, "入会消息已解析");

    const char *peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "peer_id"));
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "token"));
    const char *app_id = json_string_any(root, "wx_app_id", "wxa_app_id");
    const char *model_id = json_string_any(root, "wx_model_id", "wxa_model_id");
    const char *session_token = json_string_any(root, "wx_session_token", "wxa_session_token");
    const char *room_id = json_string_any(root, "wx_room_id", "wxa_room_id");
    const char *wx_payload = json_string_any(root, "wx_payload", "wxa_payload");

    if (session_token == NULL || session_token[0] == '\0')
    {
        session_token = json_string_any(root, "wx_server_token", "wxa_server_token");
    }

    char app_id_from_peer[64] = {0};
    char model_id_from_peer[64] = {0};
    if ((app_id == NULL || app_id[0] == '\0') && peer_id != NULL)
    {
        extract_query_param(peer_id, "x_wx_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        if (app_id_from_peer[0] == '\0')
        {
            extract_query_param(peer_id, "x_wxa_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        }
        app_id = app_id_from_peer;
    }
    if ((model_id == NULL || model_id[0] == '\0') && peer_id != NULL)
    {
        extract_query_param(peer_id, "x_wx_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        if (model_id_from_peer[0] == '\0')
        {
            extract_query_param(peer_id, "x_wxa_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        }
        model_id = model_id_from_peer;
    }

    if (peer_id == NULL || peer_id[0] == '\0' || token == NULL || token[0] == '\0')
    {
        ESP_LOGE(TAG, "来电消息缺少连接信息");
        return ESP_ERR_INVALID_ARG;
    }

    WX_VOIP_TRACEI(TAG,
                   "入会连接信息: peer_id_len=%u token_len=%u room_id=%s app_id=%s model_id=%s session_token_len=%u",
                   (unsigned)strlen(peer_id),
                   (unsigned)strlen(token),
                   room_id && room_id[0] ? room_id : "(空)",
                   app_id && app_id[0] ? app_id : "(空)",
                   model_id && model_id[0] ? model_id : "(空)",
                   session_token ? (unsigned)strlen(session_token) : 0);
    WX_VOIP_TRACEI(TAG, "准备保存入会信息");

    if (!lock_session_wait("保存入会信息", 1000))
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_session.state != VOIP_STATE_IDLE)
    {
        voip_state_t state = s_session.state;
        voip_session_t busy = {0};
        copy_str(busy.wx_app_id, sizeof(busy.wx_app_id), app_id);
        copy_str(busy.wx_model_id, sizeof(busy.wx_model_id), model_id);
        copy_str(busy.wx_session_token, sizeof(busy.wx_session_token), session_token);
        copy_str(busy.wx_room_id, sizeof(busy.wx_room_id), room_id);
        copy_str(busy.wx_payload, sizeof(busy.wx_payload), wx_payload);
        unlock_session();

        ESP_LOGW(TAG, "当前状态=%s,拒接新的微信来电", state_name(state));
        (void)reject_later(&busy, TIRTC_VOIP_HANGUP_REASON_BUSY);
        return ESP_ERR_INVALID_STATE;
    }

    clear_session_locked();
    copy_str(s_session.peer_id, sizeof(s_session.peer_id), peer_id);
    copy_str(s_session.token, sizeof(s_session.token), token);
    copy_str(s_session.wx_app_id, sizeof(s_session.wx_app_id), app_id);
    copy_str(s_session.wx_model_id, sizeof(s_session.wx_model_id), model_id);
    copy_str(s_session.wx_session_token, sizeof(s_session.wx_session_token), session_token);
    copy_str(s_session.wx_room_id, sizeof(s_session.wx_room_id), room_id);
    copy_str(s_session.wx_payload, sizeof(s_session.wx_payload), wx_payload);
    s_session.state = VOIP_STATE_RINGING;
    set_deadline_locked(VOIP_RING_TIMEOUT_MS);
    unlock_session();

    WX_VOIP_TRACEI(TAG, "入会信息已保存: auto=%d", auto_answer ? 1 : 0);

    if (auto_answer)
    {
        ESP_LOGI(TAG, "微信已接听主动呼叫,自动建立通话");
        return start_answer_task("主动呼叫接通", VOIP_ACTIVE_ANSWER_DELAY_MS);
    }

    ESP_LOGI(TAG, "收到微信来电");
    ESP_LOGI(TAG, "请按 BOOT 键接听");
    return ESP_OK;
}

esp_err_t wechat_voip_session_answer(void)
{
    ensure_init();
    return start_answer_task("BOOT 键接听", VOIP_BOOT_ANSWER_DELAY_MS);
}

esp_err_t wechat_voip_session_reject_incoming(void)
{
    ensure_init();

    lock_session();
    if (s_session.state != VOIP_STATE_RINGING)
    {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    voip_session_t snapshot = s_session;
    WX_VOIP_TRACEI(TAG,
                   "BOOT 长按拒接: room=%s state=%s",
                   snapshot.wx_room_id[0] ? snapshot.wx_room_id : "(空)",
                   state_name(snapshot.state));
    finish_session_locked();
    unlock_session();

    esp_err_t ret = reject_later(&snapshot, TIRTC_VOIP_HANGUP_REASON_REJECT);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "已拒接微信来电");
    }
    return ret;
}

bool wechat_voip_session_is_idle(void)
{
    ensure_init();

    lock_session();
    bool idle = (s_session.state == VOIP_STATE_IDLE);
    unlock_session();
    return idle;
}

bool wechat_voip_session_is_closing(void)
{
    ensure_init();

    lock_session();
    bool closing = (s_session.state == VOIP_STATE_CLOSING);
    unlock_session();
    return closing;
}

static void collect_status(voip_status_t *status)
{
    if (status == NULL)
    {
        return;
    }

    memset(status, 0, sizeof(*status));
    int64_t now_us = esp_timer_get_time();

    lock_session();
    status->state = s_session.state;
    status->hconn = s_session.hconn;
    status->answer_pending = s_answer_pending;
    status->answer_task = s_answer_worker_task;
    status->deadline_left_ms = s_session.deadline_us > 0 ? (s_session.deadline_us - now_us) / 1000 : 0;
    status->last_close_ago_ms = s_last_close_us > 0 ? (now_us - s_last_close_us) / 1000 : -1;
    unlock_session();

    portENTER_CRITICAL(&s_work_state_lock);
    status->work_busy = s_work_busy;
    status->work_type = s_work_type;
    portEXIT_CRITICAL(&s_work_state_lock);

    status->work_queue_len = s_work_queue ? (uint32_t)uxQueueMessagesWaiting(s_work_queue) : 0;
    status->media_running = wechat_voip_media_is_running();
}

static bool status_ready_for_next_call(const voip_status_t *status)
{
    return status != NULL &&
           status->state == VOIP_STATE_IDLE &&
           status->hconn == NULL &&
           !status->answer_pending &&
           status->answer_task == NULL &&
           !status->work_busy &&
           status->work_queue_len == 0 &&
           !status->media_running &&
           (status->last_close_ago_ms < 0 || status->last_close_ago_ms >= VOIP_RECALL_GUARD_MS);
}

static void log_status(const char *reason, const voip_status_t *status, bool warning)
{
    if (status == NULL)
    {
        return;
    }

    if (warning)
    {
        ESP_LOGW(TAG,
                 "通话资源%s: %s state=%s hconn=%p answer_task=%p answer_pending=%d "
                 "work_busy=%d work=%s workq=%u media=%d deadline=%lldms last_close=%lldms",
                 status_ready_for_next_call(status) ? "已就绪" : "未就绪",
                 reason ? reason : "状态检查",
                 state_name(status->state),
                 status->hconn,
                 status->answer_task,
                 status->answer_pending ? 1 : 0,
                 status->work_busy ? 1 : 0,
                 work_type_name(status->work_type),
                 (unsigned)status->work_queue_len,
                 status->media_running ? 1 : 0,
                 (long long)status->deadline_left_ms,
                 (long long)status->last_close_ago_ms);
        return;
    }

    ESP_LOGI(TAG,
             "通话资源%s: %s state=%s hconn=%p answer_task=%p answer_pending=%d "
             "work_busy=%d work=%s workq=%u media=%d deadline=%lldms last_close=%lldms",
             status_ready_for_next_call(status) ? "已就绪" : "未就绪",
             reason ? reason : "状态检查",
             state_name(status->state),
             status->hconn,
             status->answer_task,
             status->answer_pending ? 1 : 0,
             status->work_busy ? 1 : 0,
             work_type_name(status->work_type),
             (unsigned)status->work_queue_len,
             status->media_running ? 1 : 0,
             (long long)status->deadline_left_ms,
             (long long)status->last_close_ago_ms);
}

bool wechat_voip_session_ready_for_next_call(bool log_detail)
{
    ensure_init();

    voip_status_t status;
    collect_status(&status);
    bool ready = status_ready_for_next_call(&status);
    if (log_detail || !ready)
    {
        log_status("发起呼叫前检查", &status, !ready);
    }
    return ready;
}

void wechat_voip_session_dump_status(const char *reason)
{
    ensure_init();

    voip_status_t status;
    collect_status(&status);
    log_status(reason, &status, !status_ready_for_next_call(&status));
}

void wechat_voip_session_maintenance(void)
{
    ensure_init();

    int64_t now_us = esp_timer_get_time();
    lock_session();
    bool expired = (s_session.state != VOIP_STATE_IDLE &&
                    s_session.deadline_us > 0 &&
                    now_us >= s_session.deadline_us);
    voip_session_t snapshot = s_session;
    if (expired)
    {
        if (s_session.state == VOIP_STATE_CLOSING)
        {
            set_deadline_locked(VOIP_CLOSE_WAIT_TIMEOUT_MS);
        }
        else if (s_session.hconn == NULL)
        {
            finish_session_locked();
        }
        else
        {
            begin_close_locked();
        }
    }
    unlock_session();

    if (!expired)
    {
        voip_status_t status;
        collect_status(&status);
        if (!status_ready_for_next_call(&status) &&
            status.state == VOIP_STATE_IDLE &&
            now_us - s_last_status_warn_us >= (int64_t)VOIP_STATUS_WARN_INTERVAL_MS * 1000)
        {
            s_last_status_warn_us = now_us;
            log_status("空闲但资源未收干净", &status, true);
        }
        return;
    }

    WX_VOIP_TRACEW(TAG,
                   "会话超时维护: state=%s hconn=%p deadline=%lld now=%lld",
                   state_name(snapshot.state),
                   snapshot.hconn,
                   (long long)snapshot.deadline_us,
                   (long long)now_us);

    if (snapshot.state == VOIP_STATE_CLOSING)
    {
        ESP_LOGW(TAG, "微信通话关闭等待超时,重试断开");
        wechat_voip_media_stop(snapshot.hconn);
        disconnect_later(snapshot.hconn, true);
        return;
    }

    ESP_LOGW(TAG, "微信通话%s超时,准备结束", state_name(snapshot.state));
    if (snapshot.hconn != NULL)
    {
        voip_work_item_t item = {
            .type = VOIP_WORK_HANGUP,
            .hconn = snapshot.hconn,
            .reason = TIRTC_VOIP_HANGUP_REASON_TIMEOUT,
        };
        (void)enqueue_work(&item);
        return;
    }

    voip_work_item_t item = {
        .type = VOIP_WORK_REJECT,
        .reason = TIRTC_VOIP_HANGUP_REASON_TIMEOUT,
    };
    fill_reject_info(&item.reject, &snapshot);
    (void)enqueue_work(&item);
}

bool wechat_voip_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "会话命令回调: hconn=%p cmdw=0x%08x len=%u",
                   hconn,
                   (unsigned)cmdw,
                   (unsigned)len);

    if (hconn == NULL)
    {
        return false;
    }

    lock_session();
    tirtc_conn_t current = s_session.hconn;
    voip_state_t state = s_session.state;
    if (current == NULL &&
        (state == VOIP_STATE_CONNECTING || state == VOIP_STATE_AWAITING_CONNECTED) &&
        (voip_cmd_is(cmdw, TIRTC_VOIP_CALL_CONNECTED) ||
         voip_cmd_is(cmdw, TIRTC_VOIP_HANGUP)))
    {
        s_session.hconn = hconn;
        current = hconn;
        ESP_LOGD(TAG, "通话命令先于连接回调到达 hconn=%p cmdw=0x%08x",
                 hconn, (unsigned)cmdw);
    }
    unlock_session();

    if (hconn != current)
    {
        ESP_LOGD(TAG,
                 "忽略非当前通话命令 hconn=%p current=%p state=%s cmdw=0x%08x",
                 hconn,
                 current,
                 state_name(state),
                 (unsigned)cmdw);
        return false;
    }

    if (voip_cmd_is(cmdw, TIRTC_VOIP_CALL_CONNECTED))
    {
        lock_session();
        if (s_session.state == VOIP_STATE_AWAITING_CONNECTED || s_session.state == VOIP_STATE_CONNECTING)
        {
            s_session.state = VOIP_STATE_IN_CALL;
            set_deadline_locked(0);
        }
        unlock_session();

        ESP_LOGI(TAG, "微信通话已接通");
        WX_VOIP_TRACEI(TAG, "收到 CALL_CONNECTED,启动媒体: hconn=%p", hconn);
        (void)wechat_voip_media_start(hconn);
        return true;
    }

    if (voip_cmd_is(cmdw, TIRTC_VOIP_HANGUP))
    {
        int reason = -1;
        if (data != NULL && len > 0)
        {
            char buf[96];
            if (len < sizeof(buf))
            {
                memcpy(buf, data, len);
                buf[len] = '\0';
                cJSON *root = cJSON_Parse(buf);
                if (root != NULL)
                {
                    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "reason");
                    if (cJSON_IsNumber(item))
                    {
                        reason = item->valueint;
                    }
                    cJSON_Delete(root);
                }
            }
            else
            {
                ESP_LOGD(TAG, "挂断命令内容过长,跳过原因解析 len=%u", (unsigned)len);
            }
        }

        ESP_LOGI(TAG, "微信通话已结束,原因=%d,state=%s", reason, state_name(state));
        WX_VOIP_TRACEI(TAG,
                       "收到 HANGUP: hconn=%p reason=%d payload_len=%u",
                       hconn,
                       reason,
                       (unsigned)len);

        lock_session();
        if (s_session.hconn == hconn)
        {
            begin_close_locked();
        }
        else
        {
            finish_session_locked();
        }
        unlock_session();

        disconnect_later(hconn, true);
        return true;
    }

    ESP_LOGD(TAG, "通话中收到未处理命令 cmdw=0x%08x", (unsigned)cmdw);
    return true;
}

bool wechat_voip_session_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    WX_VOIP_TRACEI(TAG,
                   "会话音频回调: hconn=%p stream=%u len=%u ts=%u",
                   hconn,
                   info ? info->stream_id : 0,
                   info ? (unsigned)info->length : 0,
                   info ? (unsigned)info->ts : 0);
    return wechat_voip_media_on_audio(hconn, info, data);
}

bool wechat_voip_session_on_conn_error(tirtc_conn_t hconn, int error)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "会话连接错误回调: hconn=%p error=%d",
                   hconn,
                   error);

    lock_session();
    bool mine = (hconn != NULL && hconn == s_session.hconn);
    if (mine)
    {
        finish_session_locked();
    }
    unlock_session();

    if (!mine)
    {
        return false;
    }

    ESP_LOGW(TAG, "微信通话连接错误: %d %s", error, TiRtcGetErrorStr(error));
    wechat_voip_media_stop(hconn);
    disconnect_later(hconn, false);
    return true;
}

bool wechat_voip_session_on_disconnected(tirtc_conn_t hconn)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG, "会话断开回调: hconn=%p", hconn);

    lock_session();
    bool mine = (hconn != NULL && hconn == s_session.hconn);
    if (mine)
    {
        finish_session_locked();
    }
    unlock_session();

    if (mine)
    {
        wechat_voip_media_stop(hconn);
        ESP_LOGI(TAG, "微信通话已断开");
        wechat_voip_session_dump_status("断开回调后");
    }

    return mine;
}

bool wechat_voip_session_cancel_by_room(const char *room_id)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG, "微信取消事件: room=%s", room_id && room_id[0] ? room_id : "(空)");

    lock_session();
    bool active = (s_session.state != VOIP_STATE_IDLE);
    voip_session_t snapshot = s_session;
    bool room_match = true;
    if (active && snapshot.wx_room_id[0] != '\0')
    {
        room_match = (room_id != NULL && room_id[0] != '\0' &&
                      strcmp(snapshot.wx_room_id, room_id) == 0);
    }
    if (active && room_match)
    {
        if (s_session.hconn != NULL)
        {
            begin_close_locked();
        }
        else
        {
            finish_session_locked();
        }
    }
    unlock_session();

    if (!active)
    {
        return false;
    }
    if (!room_match)
    {
        ESP_LOGD(TAG, "忽略非当前房间取消: room_id=%s current=%s",
                 room_id ? room_id : "(空)",
                 snapshot.wx_room_id);
        return false;
    }

    if (snapshot.hconn != NULL)
    {
        wechat_voip_media_stop(snapshot.hconn);
        disconnect_later(snapshot.hconn, true);
        ESP_LOGI(TAG, "微信侧已结束当前通话");
    }
    else
    {
        ESP_LOGI(TAG, "微信侧已取消当前来电");
    }

    return true;
}

void wechat_voip_session_hangup(void)
{
    ensure_init();

    lock_session();
    bool active = (s_session.state != VOIP_STATE_IDLE);
    voip_session_t snapshot = s_session;
    WX_VOIP_TRACEI(TAG,
                   "本地请求挂断: active=%d state=%s hconn=%p",
                   active ? 1 : 0,
                   state_name(snapshot.state),
                   snapshot.hconn);
    if (active && s_session.state == VOIP_STATE_CLOSING)
    {
        unlock_session();
        ESP_LOGI(TAG, "微信通话正在关闭");
        wechat_voip_session_dump_status("重复挂断");
        return;
    }
    if (active && s_session.hconn != NULL)
    {
        begin_close_locked();
    }
    else
    {
        finish_session_locked();
    }
    unlock_session();

    if (!active)
    {
        return;
    }

    if (snapshot.hconn != NULL)
    {
        voip_work_item_t item = {
            .type = VOIP_WORK_HANGUP,
            .hconn = snapshot.hconn,
            .reason = TIRTC_VOIP_HANGUP_REASON_MANUAL,
        };
        if (enqueue_work(&item) != ESP_OK)
        {
            ESP_LOGW(TAG, "挂断任务投递失败,等待维护重试");
            wechat_voip_media_stop(snapshot.hconn);
        }
        return;
    }

    tirtc_voip_hangup_reason_t reason = snapshot.state == VOIP_STATE_RINGING
                                             ? TIRTC_VOIP_HANGUP_REASON_REJECT
                                             : TIRTC_VOIP_HANGUP_REASON_MANUAL;
    voip_work_item_t item = {
        .type = VOIP_WORK_REJECT,
        .reason = reason,
    };
    fill_reject_info(&item.reject, &snapshot);
    if (enqueue_work(&item) == ESP_OK)
    {
        ESP_LOGI(TAG, "%s", snapshot.state == VOIP_STATE_RINGING ? "已拒接微信来电" : "已取消微信通话");
    }
}
