#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_version.h"
#include "boot_button.h"
#include "time_sync.h"
#include "tirtc_app.h"
#include "wechat_voip_session.h"
#include "wechat_voip_trace.h"
#include "wechat_voip_ws.h"
#include "wifi_sta.h"

static const char *TAG = "app_main";
static TaskHandle_t s_boot_action_task;
static QueueHandle_t s_boot_action_queue;
static portMUX_TYPE s_boot_intent_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_pending_boot_call;

enum
{
    BOOT_ACTION_TASK_STACK = 12288,
    BOOT_ACTION_TASK_PRIORITY = 5,
    BOOT_ACTION_QUEUE_LEN = 8,
};

/*
 * BOOT 键只表达用户意图.
 * 如果通话正在关闭或网络暂未就绪,先记下“稍后呼叫”,等资源收干净后再执行.
 */
static bool pending_boot_call_active(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_boot_intent_lock);
    pending = s_pending_boot_call;
    portEXIT_CRITICAL(&s_boot_intent_lock);
    return pending;
}

static void set_pending_boot_call(const char *reason)
{
    bool already_pending = false;
    portENTER_CRITICAL(&s_boot_intent_lock);
    already_pending = s_pending_boot_call;
    s_pending_boot_call = true;
    portEXIT_CRITICAL(&s_boot_intent_lock);

    if (!already_pending)
    {
        ESP_LOGI(TAG, "通话正在收尾,稍后自动发起微信呼叫: %s", reason ? reason : "等待资源就绪");
    }
    else
    {
        WX_VOIP_TRACEI(TAG, "微信呼叫已在等待队列中: %s", reason ? reason : "等待资源就绪");
    }
}

static void clear_pending_boot_call(const char *reason)
{
    bool was_pending = false;
    portENTER_CRITICAL(&s_boot_intent_lock);
    was_pending = s_pending_boot_call;
    s_pending_boot_call = false;
    portEXIT_CRITICAL(&s_boot_intent_lock);

    if (was_pending)
    {
        WX_VOIP_TRACEI(TAG, "清除待发微信呼叫: %s", reason ? reason : "已处理");
    }
}

static esp_err_t start_wechat_call_now(const char *source)
{
    if (!wechat_voip_ws_is_connected())
    {
        set_pending_boot_call("业务连接未就绪");
        return ESP_ERR_INVALID_STATE;
    }

    if (!wechat_voip_session_ready_for_next_call(true))
    {
        set_pending_boot_call("通话资源未就绪");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "%s", source ? source : "发起微信呼叫");
    esp_err_t ret = wechat_voip_request_call();
    if (ret == ESP_OK)
    {
        clear_pending_boot_call("呼叫已发起");
        return ESP_OK;
    }

    if (ret == ESP_ERR_INVALID_STATE)
    {
        set_pending_boot_call("呼叫条件暂未满足");
    }
    else
    {
        clear_pending_boot_call("呼叫发起失败");
    }
    ESP_LOGW(TAG, "微信呼叫未发起: %s", esp_err_to_name(ret));
    return ret;
}

static void process_pending_boot_call(void)
{
    wechat_voip_session_maintenance();
    wechat_voip_ws_maintenance();

    if (!pending_boot_call_active())
    {
        return;
    }

    if (wechat_voip_request_call_busy() ||
        !wechat_voip_ws_is_connected() ||
        !wechat_voip_session_ready_for_next_call(false))
    {
        return;
    }

    (void)start_wechat_call_now("收尾完成,自动发起微信呼叫");
}

static void handle_boot_short_press(void)
{
    wechat_voip_session_maintenance();
    wechat_voip_ws_maintenance();
    wechat_voip_session_dump_status("BOOT 短按");

    esp_err_t ret = wechat_voip_session_answer();
    if (ret == ESP_OK)
    {
        clear_pending_boot_call("已接听微信来电");
        ESP_LOGI(TAG, "已接听微信来电");
        return;
    }
    if (ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "微信来电接听失败: %s", esp_err_to_name(ret));
        return;
    }

    if (wechat_voip_session_is_closing())
    {
        set_pending_boot_call("通话正在关闭");
        return;
    }

    if (!wechat_voip_session_is_idle())
    {
        clear_pending_boot_call("用户挂断当前通话");
        ESP_LOGI(TAG, "BOOT 键挂断当前微信通话");
        wechat_voip_session_hangup();
        return;
    }

    if (wechat_voip_request_call_busy())
    {
        clear_pending_boot_call("取消主动呼叫等待");
        ESP_LOGI(TAG, "BOOT 键取消本地主动呼叫等待");
        wechat_voip_cancel_pending_call();
        return;
    }

    (void)start_wechat_call_now("BOOT 键发起微信呼叫");
}

static void handle_boot_long_press(void)
{
    wechat_voip_session_maintenance();
    wechat_voip_ws_maintenance();
    wechat_voip_session_dump_status("BOOT 长按");
    clear_pending_boot_call("BOOT 长按");

    esp_err_t ret = wechat_voip_session_reject_incoming();
    if (ret == ESP_OK)
    {
        return;
    }
    if (ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "微信来电拒接失败: %s", esp_err_to_name(ret));
        return;
    }

    if (!wechat_voip_session_is_idle())
    {
        ESP_LOGI(TAG, "BOOT 键长按,挂断当前微信通话");
        wechat_voip_session_hangup();
        return;
    }

    if (wechat_voip_request_call_busy())
    {
        ESP_LOGI(TAG, "BOOT 键长按,取消本地主动呼叫等待");
        wechat_voip_cancel_pending_call();
        return;
    }

    ESP_LOGI(TAG, "BOOT 键长按,当前没有微信通话");
}

static void boot_action_task(void *arg)
{
    (void)arg;

    /* 用队列保存快按事件,避免连续短按被 task notify overwrite 合并掉. */
    while (true)
    {
        boot_button_event_t event;
        if (xQueueReceive(s_boot_action_queue, &event, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (event == BOOT_BUTTON_EVENT_LONG_PRESS)
        {
            WX_VOIP_TRACEI(TAG, "BOOT 业务任务收到长按事件");
            handle_boot_long_press();
        }
        else
        {
            WX_VOIP_TRACEI(TAG, "BOOT 业务任务收到短按事件");
            handle_boot_short_press();
        }
    }
}

static void on_boot_button_event(boot_button_event_t event, void *user_data)
{
    (void)user_data;

    if (s_boot_action_queue == NULL)
    {
        ESP_LOGW(TAG, "BOOT 键业务任务未就绪");
        return;
    }

    WX_VOIP_TRACEI(TAG,
                   "BOOT 回调投递事件: event=%s",
                   event == BOOT_BUTTON_EVENT_LONG_PRESS ? "long" : "short");
    if (xQueueSend(s_boot_action_queue, &event, 0) != pdPASS)
    {
        ESP_LOGW(TAG, "BOOT 键事件队列已满,丢弃本次按键");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "微信 VoIP 示例启动: %s v%s,发布=%s,TiRTC=%s",
             APP_DEMO_NAME,
             APP_DEMO_VERSION,
             APP_DEMO_RELEASE_DATE,
             APP_DEMO_TIRTC_SDK_VERSION);

    if (wifi_sta_connect() != ESP_OK)
    {
        ESP_LOGE(TAG, "Wi-Fi 连接失败,中止启动");
        return;
    }

    if (time_sync_once() != ESP_OK)
    {
        ESP_LOGE(TAG, "系统时间同步失败,中止启动");
        return;
    }

    if (tirtc_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "TiRTC 启动失败,请检查 tirtc_config.h 中的设备 ID 和密钥");
        return;
    }

    if (wechat_voip_ws_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "业务连接启动失败");
        return;
    }

    s_boot_action_queue = xQueueCreate(BOOT_ACTION_QUEUE_LEN, sizeof(boot_button_event_t));
    if (s_boot_action_queue == NULL)
    {
        ESP_LOGE(TAG, "BOOT 键事件队列创建失败");
        return;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(boot_action_task,
                                              "boot_action",
                                              BOOT_ACTION_TASK_STACK,
                                              NULL,
                                              BOOT_ACTION_TASK_PRIORITY,
                                              &s_boot_action_task,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "BOOT 键业务任务创建失败");
        return;
    }

    ESP_ERROR_CHECK(boot_button_start(on_boot_button_event, NULL));
    ESP_LOGI(TAG, "启动完成,等待微信来电或 BOOT 键呼叫");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        process_pending_boot_call();
    }
}
