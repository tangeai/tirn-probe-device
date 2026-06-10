/*
 * 微信 VoIP 示例媒体.
 *
 * 这里不接真实采集和播放,先把根目录 send_audio.wav 作为示例语音
 * 循环发送. 接入真实音频时, 替换本文件的数据来源即可.
 */
#include "wechat_voip_media.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "wechat_voip_trace.h"

extern const uint8_t g_send_audio_wav_start[] asm("_binary_send_audio_wav_start");
extern const uint8_t g_send_audio_wav_end[] asm("_binary_send_audio_wav_end");

static const char *TAG = "voip_media";

enum
{
    AUDIO_STREAM_ID = 0,
    AUDIO_RATE_HZ = 8000,
    PCMA_PACKET_BYTES = 160,
    PCMA_PACKET_DURATION_MS = 20,
    MEDIA_TASK_STACK = 8192,
    MEDIA_TASK_PRIORITY = 6,
};

static portMUX_TYPE s_media_lock = portMUX_INITIALIZER_UNLOCKED;
static tirtc_conn_t s_media_conn;
static TaskHandle_t s_media_task;
static bool s_audio_restart_requested;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint8_t pcm16_to_alaw(int16_t sample)
{
    int sign = 0;
    int exponent = 0;
    int mantissa = 0;

    if (sample < 0)
    {
        sign = 0x80;
        sample = (int16_t)(~sample);
    }

    if (sample < 256)
    {
        exponent = 0;
        mantissa = sample >> 4;
    }
    else if (sample < 512)
    {
        exponent = 1;
        mantissa = (sample >> 5) & 0x0F;
    }
    else if (sample < 1024)
    {
        exponent = 2;
        mantissa = (sample >> 6) & 0x0F;
    }
    else if (sample < 2048)
    {
        exponent = 3;
        mantissa = (sample >> 7) & 0x0F;
    }
    else if (sample < 4096)
    {
        exponent = 4;
        mantissa = (sample >> 8) & 0x0F;
    }
    else if (sample < 8192)
    {
        exponent = 5;
        mantissa = (sample >> 9) & 0x0F;
    }
    else if (sample < 16384)
    {
        exponent = 6;
        mantissa = (sample >> 10) & 0x0F;
    }
    else
    {
        exponent = 7;
        mantissa = (sample >> 11) & 0x0F;
    }

    return (uint8_t)(sign | (exponent << 4) | mantissa) ^ 0x55U;
}

static bool parse_wav_pcm16_mono(const uint8_t *wav,
                                 size_t wav_size,
                                 const uint8_t **pcm,
                                 size_t *pcm_bytes,
                                 uint32_t *sample_rate)
{
    if (wav == NULL || wav_size < 44 || pcm == NULL || pcm_bytes == NULL || sample_rate == NULL)
    {
        return false;
    }

    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
    {
        return false;
    }

    bool fmt_ok = false;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    size_t data_size = 0;
    size_t pos = 12;
    while (pos + 8 <= wav_size)
    {
        const uint8_t *chunk = wav + pos;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t payload = pos + 8;
        if (payload + chunk_size > wav_size)
        {
            break;
        }

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16)
        {
            uint16_t audio_format = read_le16(wav + payload);
            uint16_t channels = read_le16(wav + payload + 2);
            uint32_t sample_rate = read_le32(wav + payload + 4);
            uint16_t bits_per_sample = read_le16(wav + payload + 14);
            fmt_ok = (audio_format == 1 &&
                      channels == 1 &&
                      (sample_rate == AUDIO_RATE_HZ ||
                       sample_rate == AUDIO_RATE_HZ * 2U) &&
                      bits_per_sample == 16);
            rate = sample_rate;
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            data = wav + payload;
            data_size = chunk_size;
        }

        pos = payload + chunk_size + (chunk_size & 1U);
    }

    if (!fmt_ok || data == NULL || data_size < 2)
    {
        return false;
    }

    *pcm = data;
    *pcm_bytes = data_size & ~(size_t)1U;
    *sample_rate = rate;
    return true;
}

static tirtc_conn_t current_conn(bool *restart_audio)
{
    tirtc_conn_t conn;
    portENTER_CRITICAL(&s_media_lock);
    conn = s_media_conn;
    if (restart_audio != NULL)
    {
        *restart_audio = s_audio_restart_requested;
        s_audio_restart_requested = false;
    }
    portEXIT_CRITICAL(&s_media_lock);
    return conn;
}

static void pcma_task(void *arg)
{
    (void)arg;

    const uint8_t *wav_data = g_send_audio_wav_start;
    const size_t wav_size = (size_t)(g_send_audio_wav_end - g_send_audio_wav_start);
    const uint8_t *pcm_data = NULL;
    size_t pcm_size = 0;
    uint32_t wav_sample_rate = AUDIO_RATE_HZ;
    size_t wav_step_bytes = 2;
    TickType_t last_wake = xTaskGetTickCount();
    size_t audio_offset = 0;
    uint32_t media_ts_ms = 0;
    uint32_t tx_count = 0;
    uint32_t tx_drop = 0;
    uint32_t tx_busy = 0;
    uint8_t packet[PCMA_PACKET_BYTES];

    if (!parse_wav_pcm16_mono(wav_data, wav_size, &pcm_data, &pcm_size, &wav_sample_rate))
    {
        ESP_LOGE(TAG, "send_audio.wav 必须是 8kHz 或 16kHz, 16bit, mono PCM");
        goto exit_task;
    }

    /* 微信语音通话按 8k PCMA 发送, 16k 提示音在这里轻量降到 8k. */
    wav_step_bytes = (wav_sample_rate / AUDIO_RATE_HZ) * 2U;

    ESP_LOGI(TAG, "示例音频开始发送: %uHz, %ums/包",
             AUDIO_RATE_HZ,
             PCMA_PACKET_DURATION_MS);
    WX_VOIP_TRACEI(TAG,
                   "示例音频任务启动: wav=%u pcm=%u source_rate=%u step=%u packet=%u",
                   (unsigned)wav_size,
                   (unsigned)pcm_size,
                   (unsigned)wav_sample_rate,
                   (unsigned)wav_step_bytes,
                   (unsigned)PCMA_PACKET_BYTES);

    while (true)
    {
        bool restart_audio = false;
        tirtc_conn_t conn = current_conn(&restart_audio);
        if (conn == NULL)
        {
            break;
        }

        if (restart_audio)
        {
            audio_offset = 0;
            media_ts_ms = 0;
            tx_count = 0;
            tx_drop = 0;
            tx_busy = 0;
            last_wake = xTaskGetTickCount();
            WX_VOIP_TRACEI(TAG, "连接切换, 示例音频重新开始: hconn=%p", conn);
        }

        size_t next_audio_offset = audio_offset;

        for (size_t copied = 0; copied < PCMA_PACKET_BYTES;)
        {
            if (next_audio_offset + 2 > pcm_size)
            {
                next_audio_offset = 0;
            }
            int16_t sample = (int16_t)read_le16(pcm_data + next_audio_offset);
            packet[copied++] = pcm16_to_alaw(sample);
            next_audio_offset += wav_step_bytes;
        }

        TIRTCFRAMEINFO frame = {
            .stream_id = AUDIO_STREAM_ID,
            .media = TIRTC_AUDIO_ALAW,
            .flags = TIRTC_AUDIOSAMPLE_8K16B1C,
            .reserved = 0,
            .ts = media_ts_ms,
            .length = PCMA_PACKET_BYTES,
        };

        int ret = TiRtcSendAudioStream(conn, &frame, packet);
        tx_count++;
        if (ret < 0)
        {
            tx_drop++;
            if (ret == TIRTC_E_BUSY)
            {
                tx_busy++;
            }
            if ((tx_count % 100U) == 1U || ret != TIRTC_E_BUSY)
            {
                ESP_LOGW(TAG, "示例音频发送受阻: ret=%d %s drop=%" PRIu32 " busy=%" PRIu32,
                         ret, TiRtcGetErrorStr(ret), tx_drop, tx_busy);
            }
            WX_VOIP_TRACEW(TAG,
                           "发送示例音频失败: ret=%d %s count=%" PRIu32 " drop=%" PRIu32 " busy=%" PRIu32,
                           ret,
                           TiRtcGetErrorStr(ret),
                           tx_count,
                           tx_drop,
                           tx_busy);
        }
        else
        {
            audio_offset = next_audio_offset;
            media_ts_ms += PCMA_PACKET_DURATION_MS;
            if (tx_count == 1U || (tx_count % 50U) == 0U)
            {
                WX_VOIP_TRACEI(TAG,
                               "已发送示例音频: hconn=%p count=%" PRIu32 " ts=%u offset=%u len=%u",
                               conn,
                               tx_count,
                               (unsigned)frame.ts,
                               (unsigned)audio_offset,
                               (unsigned)frame.length);
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PCMA_PACKET_DURATION_MS));
    }

exit_task:
    portENTER_CRITICAL(&s_media_lock);
    s_media_task = NULL;
    s_audio_restart_requested = false;
    portEXIT_CRITICAL(&s_media_lock);

    ESP_LOGI(TAG, "示例音频已停止");
    WX_VOIP_TRACEI(TAG, "示例音频任务退出");
    vTaskDelete(NULL);
}

esp_err_t wechat_voip_media_start(tirtc_conn_t hconn)
{
    if (hconn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    WX_VOIP_TRACEI(TAG, "请求启动示例音频: hconn=%p", hconn);

    portENTER_CRITICAL(&s_media_lock);
    if (s_media_task != NULL && s_media_conn == hconn)
    {
        portEXIT_CRITICAL(&s_media_lock);
        WX_VOIP_TRACEI(TAG, "示例音频已在当前连接发送: hconn=%p", hconn);
        return ESP_OK;
    }

    s_media_conn = hconn;
    s_audio_restart_requested = true;
    bool need_create = (s_media_task == NULL);
    portEXIT_CRITICAL(&s_media_lock);

    if (!need_create)
    {
        WX_VOIP_TRACEI(TAG, "示例音频切换到新连接: hconn=%p", hconn);
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(pcma_task,
                                         "voip_pcma",
                                         MEDIA_TASK_STACK,
                                         NULL,
                                         MEDIA_TASK_PRIORITY,
                                         &s_media_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        portENTER_CRITICAL(&s_media_lock);
        s_media_conn = NULL;
        s_media_task = NULL;
        s_audio_restart_requested = false;
        portEXIT_CRITICAL(&s_media_lock);
        WX_VOIP_TRACEW(TAG, "示例音频任务创建失败: hconn=%p", hconn);
        return ESP_FAIL;
    }

    WX_VOIP_TRACEI(TAG, "示例音频任务已创建: hconn=%p", hconn);
    return ESP_OK;
}

void wechat_voip_media_stop(tirtc_conn_t hconn)
{
    WX_VOIP_TRACEI(TAG, "请求停止示例音频: hconn=%p", hconn);

    portENTER_CRITICAL(&s_media_lock);
    if (hconn == NULL || hconn == s_media_conn)
    {
        s_media_conn = NULL;
        s_audio_restart_requested = false;
    }
    portEXIT_CRITICAL(&s_media_lock);
}

esp_err_t wechat_voip_media_stop_wait(tirtc_conn_t hconn, uint32_t timeout_ms)
{
    WX_VOIP_TRACEI(TAG,
                   "等待示例音频停止: hconn=%p timeout=%u",
                   hconn,
                   (unsigned)timeout_ms);
    wechat_voip_media_stop(hconn);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true)
    {
        portENTER_CRITICAL(&s_media_lock);
        TaskHandle_t task = s_media_task;
        portEXIT_CRITICAL(&s_media_lock);

        if (task == NULL)
        {
            return ESP_OK;
        }
        if (task == xTaskGetCurrentTaskHandle())
        {
            return ESP_ERR_INVALID_STATE;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0)
        {
            ESP_LOGW(TAG, "示例音频停止等待超时");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool wechat_voip_media_is_running(void)
{
    portENTER_CRITICAL(&s_media_lock);
    bool running = (s_media_task != NULL || s_media_conn != NULL);
    portEXIT_CRITICAL(&s_media_lock);
    return running;
}

bool wechat_voip_media_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, const void *data)
{
    (void)data;

    if (hconn == NULL || hconn != current_conn(NULL) || info == NULL)
    {
        WX_VOIP_TRACEI(TAG,
                       "忽略对端音频: hconn=%p current=%p info=%p",
                       hconn,
                       current_conn(NULL),
                       info);
        return false;
    }

#if TIRTC_WX_VOIP_DEBUG_LOG
    static uint32_t rx_count;
    ++rx_count;
    if (rx_count == 1U || (rx_count % 50U) == 0U)
    {
        WX_VOIP_TRACEI(TAG,
                       "收到对端音频: hconn=%p stream=%u media=%u len=%u ts=%u count=%" PRIu32,
                       hconn,
                       info->stream_id,
                       info->media,
                       (unsigned)info->length,
                       (unsigned)info->ts,
                       rx_count);
    }
#endif
    return true;
}
