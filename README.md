# TiRTC 设备端送流 Demo

这是一个设备端本地媒体循环送流 Demo：启动后等待客户端连接，连接后循环发送 `assets/audio.g711a` 和 `assets/video.h264`。

仓库已经预置默认媒体文件和 TiRTC SDK。正常情况下，克隆后不需要再下载 release 包或 SDK 包。

## ESP32-S3 微信 VoIP 示例

ESP32-S3 微信 IoT VoIP 设备端示例放在:

- [`examples/wechat_voip_esp32s3`](examples/wechat_voip_esp32s3)

该示例包含 Wi-Fi、时间同步、TiRTC 上线、业务 WebSocket、微信呼入、设备主动呼叫、接听、拒接、挂断和示例音频发送流程.

## 快速开始

平台要求：

| 平台 | 要求 |
| --- | --- |
| macOS arm64 | Xcode Command Line Tools、`make` |
| Linux x86_64 | glibc 2.35 或更高版本、GCC/Make 工具链，例如 Ubuntu 上的 `build-essential` |

```sh
./script/build.sh
./script/run_demo.sh \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key
```

## 预置内容

- `assets/audio.g711a`
- `assets/video.h264`
- `3rd/macos-arm64/`
- `3rd/linux-x86_64/`
- `3rd/packages/`

`script/build.sh` 会按当前宿主平台自动选择：

- macOS arm64 -> `3rd/macos-arm64`
- Linux x86_64 -> `3rd/linux-x86_64`

构建产物：

```text
build/macos-arm64/device_uplink_demo
build/macos-arm64/tirtc_accel_device_probe
build/macos-arm64/libTiRTC.dylib
build/macos-arm64/libtgrtc.dylib
build/linux-x86_64/device_uplink_demo
build/linux-x86_64/tirtc_accel_device_probe
```

## tirtc-accel 测试工具

`tirtc_accel_device_probe` 是面向 `tirtc-accel/whip-echo-svc` 的 WHIP 测试 device 工具。它主动调用
`TiRtcWhipConnect(peer_id, token, ...)`，因此需要业务侧提前提供 `whips://...` 形式的 `peer_id` 和连接
`token`。如果 token 是一次性的，多次建连测试时由调用方保证 token 可用。

### 建连成功率和耗时

```sh
./build/macos-arm64/tirtc_accel_device_probe connect \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --iterations 10
```

输出包含建连成功率，以及成功建连耗时的 p50/p90/p95/p99。

### 并发空闲连接压测

`idle` 模式会逐个建立指定数量的 TiRTC 连接并同时保持，不主动发送命令、音频或视频业务数据，适合测试
服务端并发连接容量。`--interval-ms` 控制相邻连接的启动间隔，达到目标连接数后按 `--duration-ms` 保持；
可使用 `Ctrl-C` 提前结束并统一断开连接。

```sh
./build/linux-x86_64/tirtc_accel_device_probe idle \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --connections 1000 \
  --interval-ms 10 \
  --duration-ms 600000
```

输出包含成功建立数、峰值并发数、保持结束时的存活连接数、异常断连数及建连耗时分位值。目标服务所用
token 必须允许重复建连；同时注意单个进程的文件描述符限制应高于目标连接数。

`idle` 模式会在 `TiRtcInit()` 前将 `TIRTC_OPT_MAX_CONNECTIONS` 设置为 `--connections`，确保 SDK 按目标
并发量规划连接资源。probe runner 镜像启动时会尝试启用 unlimited core dump；Docker hard limit 不允许时会
输出告警。需要可靠保留崩溃 core 时应显式运行：

```sh
docker run --ulimit core=-1 --ulimit nofile=65535:65535 ...
```

core 文件的实际位置仍由宿主机 `kernel.core_pattern` 决定。

`script/run_accel_probe_idle_1000.sh` 会将总连接数拆成多个同时运行的 probe 进程，避免单个进程在约 300 多个
连接时触发资源或缓冲区限制。默认每个进程最多建立 300 个连接：

```sh
CONNECTIONS=1000 \
CONNECTIONS_PER_PROCESS=300 \
DURATION_MS=600000 \
./script/run_accel_probe_idle_1000.sh
```

以上配置会同时运行 4 个 worker，连接数分别为 300、300、300、100。每个 worker 使用独立容器和日志；脚本
也可被多次同时启动，运行 ID 中包含时间与进程号，不会互相覆盖容器名或日志。按 `Ctrl-C` 会清理本次启动的
所有 worker 容器。

TGWebRTC 的 `[RTC_THREAD_STAT]` 是底层通过 `printf` 直接输出的耗时诊断信息，不受 `LOG_LEVEL` 控制。
脚本默认过滤这些日志。需要保留全部或抽样保留时，可设置：

```sh
# 保留全部
RTC_THREAD_STAT_SAMPLE_EVERY=1 ./script/run_accel_probe_idle_1000.sh

# 每 100 条保留 1 条
RTC_THREAD_STAT_SAMPLE_EVERY=100 ./script/run_accel_probe_idle_1000.sh
```

`RTC_THREAD_STAT_SAMPLE_EVERY=0` 表示全部过滤，也是默认值。每个 worker 结束时会输出被过滤的总行数。

### 设备对时和设备到服务端延迟估算

```sh
./build/macos-arm64/tirtc_accel_device_probe timesync \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --repeat 20 \
  --interval-ms 100
```

工具会重复发送对时命令，由服务端返回收到命令时的服务端 UnixNano。设备端记录发送/收到响应的本地时间，
估算设备时钟到服务端时钟的 offset，并输出 RTT、offset、设备到服务端延迟估算的 p50/p90/p95/p99。

### 音频质量测试

```sh
./build/macos-arm64/tirtc_accel_device_probe audio \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --duration-ms 10000 \
  --frame-ms 40 \
  --audio-sample-log /tmp/audio-samples.csv
```

音频测试开始前会先执行对时。测试期间设备发送带序号和发送时间的测试音频包，`whip-echo-svc` 回传服务端
收到每个测试音频包的时间并继续 echo 音频。工具输出音频首包时间、音频卡顿率、设备到服务端音频延迟、
服务端到设备 echo 延迟和端到端 echo 延迟的分位值。

`--audio-sample-log` 可选。未指定或传入空字符串时不创建文件；指定路径时写入逐包 CSV，包含轮次、帧序号、
发送与收包时间、上下行和端到端延迟、相邻回声到达间隔以及卡顿标记。

可以用 Ogg Opus 文件代替内置测试音发送，并保存服务端 echo 回来的音频：

```sh
./build/macos-arm64/tirtc_accel_device_probe audio \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --audio-input ~/Downloads/send_audio.opus \
  --audio-echo-output /tmp/received_echo.opus
```

`--audio-input` 接受标准 Ogg Opus 文件，按照文件内每个 Opus packet 的原始帧时长发送。不指定
`--duration-ms` 时发送完整文件；指定后以该时长为上限。`--audio-echo-output` 保存实际收到的 Opus echo，
并按照回包到达时间补入 20ms Opus 静音帧，因此播放器或转码后的 WAV 都会保留网络停顿。多轮测试时输出文件名
自动增加 `.iteration-N`。

### TiRTC 日志等级

两个示例程序均支持 `--log-level <level>`。等级 `1`~`5` 分别对应
error/warn/ok/info/verbose；`11`~`100` 会额外开启 WebRTC 底层日志，输出量较大且可能影响性能。
`device_uplink_demo` 默认等级为 `4`，`tirtc_accel_device_probe` 默认等级为 `3`。

## 脚本说明

### `./script/build.sh`

按当前 native 平台编译 Demo。

```sh
./script/build.sh
./script/build.sh --platform macos-arm64
./script/build.sh --platform linux-x86_64
```

`--platform` 必须和当前宿主平台一致；脚本不做交叉编译。

### `./script/run_demo.sh`

运行当前平台的 Demo。

```sh
./script/run_demo.sh \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key
```

运行前请先执行 `./script/build.sh`。

## 更新或替换 SDK

### Probe runner 镜像的 TiRTC SDK

`script/build_accel_probe_runner_image.sh` 构建镜像时会从制品库下载并分别编译两个 Linux SDK 版本：

- `tgmp-linux-standard`
- `tgmp-linux-desktop-standard`

镜像运行时默认规则：`idle` 命令使用 desktop，其余命令使用 standard。也可以通过环境变量或启动参数显式选择：

```sh
docker run --rm IMAGE \
  tirtc_accel_device_probe --tirtc-sdk desktop idle ...

docker run --rm \
  -e TIRTC_SDK_VARIANT=standard \
  IMAGE \
  tirtc_accel_device_probe connect ...
```

允许值为 `standard`、`desktop`，也接受完整名称。为保证 idle 多连接测试条件一致，`idle` 显式选择 standard
会直接报错退出。probe 启动后会打印实际 SDK variant、下载地址和 `TiRtcGetVersion()`。

可以在构建时覆盖 SDK 下载链接：

```sh
TIRTC_STANDARD_SDK_URL=https://.../standard.tgz \
TIRTC_DESKTOP_SDK_URL=https://.../desktop-standard.tgz \
PROBE_IMAGE=tirtc-accel-probe-runner:test \
./script/build_accel_probe_runner_image.sh
```

SDK 包浏览地址：

- macOS arm64: https://repo-sdk.tange-ai.com/service/rest/repository/browse/tirtc-sdks/releases/macos-arm64/
- Linux x86_64: https://repo-sdk.tange-ai.com/service/rest/repository/browse/tirtc-sdks/releases/linux-x86_64/

下载目标平台的 SDK 包后，解包到工程约定目录：

```sh
macos_sdk_tgz=3rd/packages/your-macos-sdk.tgz
linux_sdk_tgz=3rd/packages/your-linux-sdk.tgz

rm -rf 3rd/macos-arm64
mkdir -p 3rd/macos-arm64
tar -xzf "$macos_sdk_tgz" \
  -C 3rd/macos-arm64 \
  --strip-components 1

rm -rf 3rd/linux-x86_64
mkdir -p 3rd/linux-x86_64
tar -xzf "$linux_sdk_tgz" \
  -C 3rd/linux-x86_64 \
  --strip-components 1
```

## macOS 上自选 Docker 跑 Linux Demo

需要在 macOS 上临时跑 Linux x86_64 版时：

```sh
docker run --rm --platform linux/amd64 \
  -v "$PWD":/work \
  -w /work \
  ubuntu:22.04 \
  bash -lc 'apt-get update && apt-get install -y --no-install-recommends build-essential ca-certificates make && ./script/build.sh && ./script/run_demo.sh --device-id your_device_id --device-secret-key your_device_secret_key'
```
