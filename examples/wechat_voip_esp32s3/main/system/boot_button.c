/*
 * BOOT 键轮询.
 *
 * 示例工程只需要稳定区分短按和长按,轮询加去抖比中断更直观,
 * 也避免在中断上下文里碰业务代码.
 */
#include "boot_button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_config.h"
#include "wechat_voip_trace.h"

static const char *TAG = "boot_button";

enum
{
    BOOT_BUTTON_POLL_MS = 20,
    BOOT_BUTTON_DEBOUNCE_MS = 60,
    BOOT_BUTTON_LONG_PRESS_MS = 1200,
    BOOT_BUTTON_TASK_STACK = 6144,
    BOOT_BUTTON_TASK_PRIORITY = 5,
};

typedef struct
{
    boot_button_callback_t callback;
    void *user_data;
} boot_button_context_t;

static void emit_button_event(boot_button_context_t *ctx, boot_button_event_t event)
{
    WX_VOIP_TRACEI(TAG, "BOOT 键%s", event == BOOT_BUTTON_EVENT_LONG_PRESS ? "长按" : "短按");
    if (ctx->callback != NULL)
    {
        ctx->callback(event, ctx->user_data);
    }
}

static void boot_button_task(void *arg)
{
    boot_button_context_t *ctx = (boot_button_context_t *)arg;
    bool stable_pressed = false;
    bool long_press_sent = false;
    TickType_t pressed_tick = 0;

    while (true)
    {
        bool pressed = (gpio_get_level((gpio_num_t)APP_BOOT_BUTTON_GPIO) == APP_BOOT_BUTTON_ACTIVE_LEVEL);
        if (pressed != stable_pressed)
        {
            vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_DEBOUNCE_MS));
            pressed = (gpio_get_level((gpio_num_t)APP_BOOT_BUTTON_GPIO) == APP_BOOT_BUTTON_ACTIVE_LEVEL);
            if (pressed != stable_pressed)
            {
                stable_pressed = pressed;
                if (stable_pressed)
                {
                    pressed_tick = xTaskGetTickCount();
                    long_press_sent = false;
                }
                else if (!long_press_sent)
                {
                    emit_button_event(ctx, BOOT_BUTTON_EVENT_SHORT_PRESS);
                }
            }
        }

        if (stable_pressed && !long_press_sent)
        {
            TickType_t elapsed_tick = xTaskGetTickCount() - pressed_tick;
            if (elapsed_tick >= pdMS_TO_TICKS(BOOT_BUTTON_LONG_PRESS_MS))
            {
                long_press_sent = true;
                emit_button_event(ctx, BOOT_BUTTON_EVENT_LONG_PRESS);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
    }
}

esp_err_t boot_button_start(boot_button_callback_t callback, void *user_data)
{
    static boot_button_context_t button_ctx;
    static TaskHandle_t button_task;

    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (button_task != NULL)
    {
        return ESP_OK;
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << APP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&gpio_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "配置 BOOT GPIO 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    button_ctx.callback = callback;
    button_ctx.user_data = user_data;

    BaseType_t task_ret = xTaskCreateWithCaps(boot_button_task,
                                              "boot_button",
                                              BOOT_BUTTON_TASK_STACK,
                                              &button_ctx,
                                              BOOT_BUTTON_TASK_PRIORITY,
                                              &button_task,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS)
    {
        button_task = NULL;
        ESP_LOGE(TAG, "创建 BOOT 键任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT 键已就绪");
    return ESP_OK;
}
