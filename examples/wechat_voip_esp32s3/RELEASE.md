# Release Notes

## 1.0.0 - 2026-06-11

- 提供 ESP32-S3 微信 IoT VoIP 设备端示例工程.
- 包含 Wi-Fi 连接, SNTP 时间同步, TiRTC 上线和业务 WebSocket 连接流程.
- 支持微信呼入设备, BOOT 短按接听, BOOT 长按拒接.
- 支持设备空闲时 BOOT 短按主动呼叫已授权的微信联系人.
- 支持通话中 BOOT 短按或长按主动挂断.
- 支持微信侧接通前取消、接通后挂断、设备侧等待超时等状态处理.
- 使用 `send_audio.wav` 演示 `TiRtcSendAudioStream()` 发送 8 kHz PCMA 示例音频.
- 默认面向带 PSRAM 的 ESP32-S3, 业务任务栈、WebSocket 分片缓冲和大块临时消息优先使用 PSRAM.
- 提供 README、流程说明、交付检查清单和 SDK 版本信息.
