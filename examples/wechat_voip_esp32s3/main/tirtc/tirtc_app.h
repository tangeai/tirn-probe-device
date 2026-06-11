#pragma once

/* TiRTC 应用层入口:main 通过这里启动 SDK. */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tirtc_start(void);

#ifdef __cplusplus
}
#endif
