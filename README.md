# tirn_probe_device

`tirn_probe_device` 是用于联调、诊断 TiRTC WHIP 服务的设备端命令行程序。它支持建连、
并发空闲连接、时钟同步、音视频链路验证和音频弱网诊断。

仓库包含探测程序源码和通用构建入口，不捆绑 TiRTC SDK。构建前需从正式分发渠道获取与你的平台匹配的 SDK。

## 支持平台

| 平台 | 编译环境 | TiRTC SDK |
| --- | --- | --- |
| macOS arm64 | Xcode Command Line Tools、Make | macOS arm64 SDK |
| Linux x86_64 | GCC、glibc、Make | Linux x86_64 SDK |

当前构建是 native build，不支持交叉编译。其他 CPU 架构和操作系统尚未验证。

## 准备 TiRTC SDK

从 TiRTC SDK 的正式分发渠道获取与你的平台兼容、且允许你使用的 SDK。解压后的目录结构应为：

```text
<sdk-dir>/
├── include/tirtc/basedef.h
├── include/tirtc/tiRTC.h
└── lib/
```

macOS SDK 的 `lib/` 需包含 `libTiRTC.dylib` 和 `libtgrtc.dylib`；Linux SDK 需包含
`libTiRTC.a`。

SDK 默认放在 `third_party/tirtc/<platform>`，该目录被 Git 忽略。也可以在构建时选择任意目录：

```sh
./script/build.sh --sdk-dir /absolute/path/to/tirtc-sdk
```

或：

```sh
TIRTC_SDK_DIR=/absolute/path/to/tirtc-sdk ./script/build.sh
```

## 构建

```sh
./script/build.sh
```

也可以直接使用 Make：

```sh
make TIRTC_SDK_DIR=/absolute/path/to/tirtc-sdk
```

主要构建产物：

```text
build/macos-arm64/tirn_probe_device
build/linux-x86_64/tirn_probe_device
```

为了兼容旧命令，构建目录同时创建 `tirn-probe-device` 符号链接。macOS 构建会把运行所需的
TiRTC 动态库复制到可执行文件旁边。

## 使用

所有命令都需要 TiRTC 接入地址、设备凭据，以及业务侧签发的 WHIP `peer_id` 和 `token`。
不要把真实凭据写进代码、脚本或提交到 Git。命令行中的密钥可能被 shell history 或进程列表记录；
实际使用建议通过环境变量提供连接参数：

```sh
export TIRTC_ENDPOINT=https://your-access.example.com
export TIRTC_DEVICE_ID=your_device_id
export TIRTC_DEVICE_SECRET_KEY=your_device_secret_key
export TIRTC_PEER_ID='whips://your-service?_tg_mode=echo'
export TIRTC_TOKEN=your_connect_token
./build/macos-arm64/tirn_probe_device connect
```

命令行参数优先于同名环境变量。执行结束后可使用 `unset` 清理敏感变量；在共享机器上还应遵循操作系统的
进程环境和凭据管理策略。

```sh
./build/macos-arm64/tirn_probe_device --help
```

### Client mode 与本地 mock

传入 `--client-mode` 时，程序使用 `TiRtcStart(NULL, ...)` 启动，不向 endpoint 请求 `/v1/start`，
但仍然强制要求 `device-id` 和 `device-secret-key`，并把 secret key 和由 device ID 生成的 client ID
设置进 SDK。随后 `TiRtcWhipConnect()` 仍会向 endpoint 请求 `/v1/connect`。

该模式适合用本地 access mock 返回 WHIP endpoint，从而不依赖真实 `tirtc-access-svc`：

```sh
./build/macos-arm64/tirn_probe_device connect \
  --client-mode \
  --endpoint http://127.0.0.1:8765 \
  --device-id local_probe \
  --device-secret-key local_secret \
  --peer-id local_whip_echo \
  --token mock_token
```

本地仍需提供 `/v1/connect` mock 和能够处理 offer/answer 的 WHIP 服务。`--client-mode` 只跳过
设备注册阶段的 `/v1/start`，不会绕过 `/v1/connect` 或 WHIP 建连。

### 音视频快速联调

`media` 发送内置 440 Hz PCM 音频和内置 1×1 JPEG 测试帧，并统计服务端返回的音视频帧。
四项计数均大于零时测试通过，不需要摄像头、麦克风或外部媒体文件。

```sh
./build/macos-arm64/tirn_probe_device media \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://your-service?_tg_mode=echo' \
  --token your_connect_token \
  --duration-sec 10
```

### 建连诊断

```sh
./build/macos-arm64/tirn_probe_device connect \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://your-service?_tg_mode=echo' \
  --token your_connect_token \
  --iterations 10
```

输出建连成功率和成功建连耗时的 p50/p90/p95/p99。

### 并发空闲连接

```sh
./build/linux-x86_64/tirn_probe_device idle \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://your-service?_tg_mode=echo' \
  --token your_connect_token \
  --connections 100 \
  --interval-ms 10 \
  --duration-sec 60
```

`idle` 不主动发送业务数据。请确保进程文件描述符上限高于目标连接数，并确认测试服务允许相同凭据并发建连。

### 时钟同步

```sh
./build/macos-arm64/tirn_probe_device timesync \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://your-service?_tg_mode=echo' \
  --token your_connect_token \
  --repeat 20
```

此命令需要目标服务实现本工具约定的对时命令响应。

### 音频诊断

```sh
./build/macos-arm64/tirn_probe_device audio \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://your-service?_tg_mode=echo' \
  --token your_connect_token \
  --duration-sec 10 \
  --audio-sample-log /tmp/audio-samples.csv
```

`audio` 可使用内置测试音，也支持 `--audio-input <ogg-opus-path>` 和
`--audio-echo-output <ogg-opus-path>`。单向延迟和音频回声指标依赖目标服务实现本工具约定的
时间戳与回声行为；它们不是通用 WHIP 标准能力。

## 便携性边界

- 源码使用 C11、POSIX 线程和 POSIX 时间/信号 API。
- 当前只为 macOS arm64 和 Linux x86_64 提供构建规则。
- TiRTC SDK 是外部必需依赖，其 ABI、系统库和再分发条款由 SDK 包决定。
- Linux 当前静态链接 `libTiRTC.a`，还依赖 `pthread`、`m` 和 `dl`。
- macOS 当前动态链接 TiRTC，并通过 `@executable_path` 查找随程序复制的动态库。
- `timesync`、`media` 的下行验证和 `audio` 的详细指标要求服务端支持相应诊断协议。
