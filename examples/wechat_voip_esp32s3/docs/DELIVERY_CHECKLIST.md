# 交付检查清单

## 工程内容

- `components/tirtc_sdk/include/`: TiRTC 头文件.
- `components/tirtc_sdk/lib/esp32s3/libTiRTC.a`: ESP32-S3 静态库.
- `main/`: 设备端示例代码.
- `send_audio.wav`: 示例提示音, 编译时嵌入固件.
- `README.md`, `VERSION.md`, `RELEASE.md`, `docs/`: 说明文档.

## 使用前准备

- ESP32-S3 模组或开发板, 建议带 8 MB PSRAM.
- ESP-IDF 5.5.4 Windows PowerShell 环境.
- 可联网 Wi-Fi.
- TiRTC 设备 ID 和设备密钥.
- 业务服务器, 需要支持设备 WebSocket, `query_auth`, `wxa_join_voip_room`, `POST /tirtc/voip/device/call`; 接通前取消建议推 `wxa_user_cancel`.
- 微信小程序侧需要完成 `wx.requestDeviceVoIP` 授权, 并通过 `report-auth` 把 `device_id/openid/model_id/wx_app_id` 写入业务服务器.
- 服务端和小程序端示例可参考 `https://github.com/tangeai/tirtc-server-example`, 服务端说明见 `weixin-voip-server/README.md`.

## 发出前检查

- `main/app_config.h` 不包含实际 Wi-Fi 账号密码.
- `main/tirtc/tirtc_config.h` 不包含实际设备 ID 或设备密钥.
- `TIRTC_WX_VOIP_DEBUG_LOG` 默认保持 `0`; 需要排查协议或回调时再临时改成 `1`.
- `sdkconfig.defaults` 保持 PSRAM 开启, 业务任务栈和大块缓冲优先使用 PSRAM.
- `idf.py build` 可以通过.
- `components/tirtc_sdk/VERSION.md` 中的 MD5 和文件大小与 `libTiRTC.a` 一致.
- 压缩包中不包含 `build/`, `.build/`, `.vscode/`, `managed_components/`, `sdkconfig`.
- 示例版本、发布日期和启动日志与本次交付一致.

## 最小运行链路

1. 填入 Wi-Fi, 设备 ID, 设备密钥和业务 WebSocket 地址.
2. 编译并烧录固件.
3. 启动日志能看到 PSRAM 容量和剩余空间.
4. 设备上线后连接业务 WebSocket.
5. 小程序完成设备 VoIP 授权并上报 `report-auth`.
6. 设备收到 `auth_list` 或 `auth_update` 后缓存联系人.
7. 微信呼入时短按 BOOT 接听, 长按 BOOT 拒接; 设备空闲时短按 BOOT 请求主动呼叫微信.

## 状态验证

- 微信呼入后短按 BOOT, 能进入 `TiRtcWhipConnect()` 并收到 `CALL_CONNECTED`.
- 微信呼入后长按 BOOT, 日志出现 `已拒接微信来电`, 设备回到空闲.
- 通话中再次按 BOOT, 设备先停止示例音频, 再发送 `TIRTC_VOIP_HANGUP` 并释放连接.
- 设备空闲时按 BOOT, 日志出现 `正在请求微信呼叫` 和 `微信呼叫请求已提交,等待微信接听,接听后设备自动入会`.
- 主动呼叫请求阶段常规日志能看到 `正在请求微信呼叫` 和 HTTP 结果; 打开 debug 后能看到 `主动呼叫 HTTP 阶段: ...`.
- 微信接听设备主动呼叫后, 如果业务服务器下发 `wxa_join_voip_room`, 设备自动建立通话.
- 微信接通前取消设备主动呼叫时, 如果业务侧推取消消息, 设备收到 `微信侧已取消当前主动呼叫等待` 并回到空闲.
- 微信接听后再拒接或挂断时, 设备收到 `微信通话已结束,原因=...` 并回到空闲.
- 主动呼叫 HTTP 请求中或等待微信接听时再次按 BOOT, 日志出现 `已取消本地主动呼叫等待`.
- 微信未接听前设备取消主动呼叫时, 固件只退出本地等待; 若需要微信侧立即停止响铃, 业务服务器需要提供设备取消主动呼叫接口.
- 上一通挂断后再次按 BOOT, 可以重新发起主动呼叫, 不应卡在中间状态.
- 上一通收到 `微信通话已断开` 后, 约 5 s 内再次按 BOOT 会提示稍后再呼叫; 保护结束后应可重新发起.
- 呼入未接听、主动呼叫等待、WHIP 未完成、微信呼入等待 `CALL_CONNECTED` 时, 超时后都应回到空闲.
