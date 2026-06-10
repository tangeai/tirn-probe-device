# WeChat VoIP ESP32-S3 Demo Version

| Field | Value |
|---|---|
| 示例版本 | 1.0.0 |
| 发布日期 | 2026-06-11 |
| TiRTC SDK | 0.1.4 |
| 目标芯片 | ESP32-S3 |
| 芯片原厂 | Espressif Systems / 乐鑫 |
| 目标 OS | FreeRTOS / ESP-IDF |
| ESP-IDF | 5.5.4 |
| 工具链 | xtensa-esp32s3-elf-gcc-14.2.0_20260121 |

## SDK 文件

| Field | Value |
|---|---|
| 文件 | `components/tirtc_sdk/lib/esp32s3/libTiRTC.a` |
| MD5 | `8476fa21b46c26ad4cbb14d7bccb887b` |
| 文件大小 | 4,946,430 bytes / 4.72 MiB |

## 说明

本版本提供微信 IoT VoIP 的 ESP32-S3 设备端接入示例. 示例包含 Wi-Fi、时间同步、TiRTC 上线、业务 WebSocket、微信呼入、设备主动呼叫、接听、拒接、挂断和示例音频发送流程.

示例媒体会把根目录 `send_audio.wav` 嵌入固件, 运行时编码成 8 kHz PCMA, 并循环调用 `TiRtcSendAudioStream()`.

工程默认面向带 PSRAM 的 ESP32-S3, 业务任务栈、WebSocket 分片缓冲和大块临时消息优先使用 PSRAM.
