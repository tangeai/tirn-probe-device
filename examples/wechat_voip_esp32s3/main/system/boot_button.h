#pragma once

/* BOOT 键监听:把短按和长按转换成应用层事件. */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    BOOT_BUTTON_EVENT_SHORT_PRESS,
    BOOT_BUTTON_EVENT_LONG_PRESS,
} boot_button_event_t;

typedef void (*boot_button_callback_t)(boot_button_event_t event, void *user_data);

esp_err_t boot_button_start(boot_button_callback_t callback, void *user_data);

#ifdef __cplusplus
}
#endif
