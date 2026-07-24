# tirn-probe-device

`tirn-probe-device` 是用于诊断、探测 TiRTC WHIP 服务的设备端命令行程序。仓库只构建并输出该程序，
不包含产品设备 Demo 或业务示例。

## 快速开始

平台要求：

| 平台 | 要求 |
| --- | --- |
| macOS arm64 | Xcode Command Line Tools、`make` |
| Linux x86_64 | glibc 2.35 或更高版本、GCC/Make 工具链，例如 Ubuntu 上的 `build-essential` |

```sh
./script/build.sh
./build/macos-arm64/tirn-probe-device --help
```

## 预置内容

- `3rd/macos-arm64/`
- `3rd/linux-x86_64/`
- `3rd/packages/`

`script/build.sh` 会按当前宿主平台自动选择：

- macOS arm64 -> `3rd/macos-arm64`
- Linux x86_64 -> `3rd/linux-x86_64`

构建产物：

```text
build/macos-arm64/tirn-probe-device
build/macos-arm64/libTiRTC.dylib
build/macos-arm64/libtgrtc.dylib
build/linux-x86_64/tirn-probe-device
```

## WHIP 诊断能力

`tirn-probe-device` 主动调用
`TiRtcWhipConnect(peer_id, token, ...)`，因此需要业务侧提前提供 `whips://...` 形式的 `peer_id` 和连接
`token`。如果 token 是一次性的，多次建连测试时由调用方保证 token 可用。

### 建连成功率和耗时

```sh
./build/macos-arm64/tirn-probe-device connect \
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
服务端并发连接容量。`--interval-ms` 控制相邻连接的启动间隔，达到目标连接数后按 `--duration-sec` 保持；
可使用 `Ctrl-C` 提前结束并统一断开连接。

```sh
./build/linux-x86_64/tirn-probe-device idle \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --connections 1000 \
  --interval-ms 10 \
  --duration-sec 600
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

`script/run_tirn_probe_idle_1000.sh` 会将总连接数拆成多个同时运行的 probe 进程，避免单个进程在约 300 多个
连接时触发资源或缓冲区限制。默认每个进程最多建立 300 个连接：

```sh
CONNECTIONS=1000 \
CONNECTIONS_PER_PROCESS=300 \
DURATION_MS=600000 \
./script/run_tirn_probe_idle_1000.sh
```

以上配置会同时运行 4 个 worker，连接数分别为 300、300、300、100。每个 worker 使用独立容器和日志；脚本
也可被多次同时启动，运行 ID 中包含时间与进程号，不会互相覆盖容器名或日志。按 `Ctrl-C` 会清理本次启动的
所有 worker 容器。

TGWebRTC 的 `[RTC_THREAD_STAT]` 是底层通过 `printf` 直接输出的耗时诊断信息，不受 `LOG_LEVEL` 控制。
脚本默认过滤这些日志。需要保留全部或抽样保留时，可设置：

```sh
# 保留全部
RTC_THREAD_STAT_SAMPLE_EVERY=1 ./script/run_tirn_probe_idle_1000.sh

# 每 100 条保留 1 条
RTC_THREAD_STAT_SAMPLE_EVERY=100 ./script/run_tirn_probe_idle_1000.sh
```

`RTC_THREAD_STAT_SAMPLE_EVERY=0` 表示全部过滤，也是默认值。每个 worker 结束时会输出被过滤的总行数。

### 设备对时和设备到服务端延迟估算

```sh
./build/macos-arm64/tirn-probe-device timesync \
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
./build/macos-arm64/tirn-probe-device audio \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --duration-sec 10 \
  --frame-ms 40 \
  --audio-sample-log /tmp/audio-samples.csv
```

音频测试开始前会先执行对时。测试帧将 `frame_info.ts` 解释为私有的 `20-bit time + 12-bit frame_index`
格式：高 20 位是 Unix 微秒时间除以 100 后的低 20 位，低 12 位是帧号。Probe 发送时编码换算到服务端
时钟域的预计发送时间；`whip-echo-svc` 回声时保留帧号并将高 20 位替换为服务端实际接收时间。时间字段
每 104.8576 秒回绕，20ms 帧的帧号每 81.92 秒回绕，因此回声 RTT 必须小于 81.92 秒。

Probe 使用发送时间到回声接收时间组成的窗口并向两端各放宽 2000ms，枚举相邻时间周期。只有唯一候选
才恢复服务端绝对接收时间；无候选、多个候选、负单向延迟、帧号歧义、重复包和未知包均保留诊断状态，
不会猜测或生成错误的单向延迟。建议最大单向延迟 30 秒，回声排空最长等待 75 秒。

`--audio-sample-log` 可选。未指定或传入空字符串时不创建文件；指定路径时写入逐包原始 CSV，包含轮次、
帧序号、帧时长、发送/回声 packed ts、微秒时间、单调时钟、对时结果及解码状态。Probe 不计算音频质量
指标；上下行延迟、端到端延迟、帧间隔、回声率和卡顿均由报告生成工具从 CSV 计算。

可以用 Ogg Opus 文件代替内置测试音发送，并保存服务端 echo 回来的音频：

```sh
./build/macos-arm64/tirn-probe-device audio \
  --endpoint https://your-access.example.com \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key \
  --peer-id 'whips://whip-echo-svc?device_id=your_device_id' \
  --token your_connect_token \
  --audio-input ~/Downloads/send_audio.opus \
  --audio-echo-output /tmp/received_echo.opus
```

`--audio-input` 接受标准 Ogg Opus 文件，按照文件内每个 Opus packet 的原始帧时长发送。不指定
`--duration-sec` 时发送完整文件；指定后以该时长为上限。`--audio-echo-output` 保存实际收到的 Opus echo，
并按照回包到达时间补入 20ms Opus 静音帧，因此播放器或转码后的 WAV 都会保留网络停顿。多轮测试时输出文件名
自动增加 `.iteration-N`。

Probe runner 镜像内置同一份语音素材的 8 kHz 和 16 kHz 单声道版本，均使用 20ms Opus 帧：

```text
/opt/tirtc-probe-tests/audio/send_audio_8k.opus
/opt/tirtc-probe-tests/audio/send_audio_16k.opus
```

对应环境变量为 `TIRTC_PROBE_AUDIO_8K` 和 `TIRTC_PROBE_AUDIO_16K`；自动化音频测试默认选用 16 kHz 版本。

### TiRTC 日志等级

程序支持 `--log-level <level>`。等级 `1`~`5` 分别对应
error/warn/ok/info/verbose；`11`~`100` 会额外开启 WebRTC 底层日志，输出量较大且可能影响性能。
默认等级为 `3`。

## 脚本说明

### `./script/build.sh`

按当前 native 平台编译 `tirn-probe-device`。

```sh
./script/build.sh
./script/build.sh --platform macos-arm64
./script/build.sh --platform linux-x86_64
```

`--platform` 必须和当前宿主平台一致；脚本不做交叉编译。

## 更新或替换 SDK

### Probe runner 镜像的 TiRTC SDK

`script/build_tirn_probe_runner_image.sh` 构建镜像时会从制品库下载并分别编译两个 Linux SDK 版本：

- `tgmp-linux-standard`
- `tgmp-linux-desktop-standard`

镜像运行时默认规则：`idle` 命令使用 desktop，其余命令使用 standard。也可以通过环境变量或启动参数显式选择：

```sh
docker run --rm IMAGE \
  tirn-probe-device --tirtc-sdk desktop idle ...

docker run --rm \
  -e TIRTC_SDK_VARIANT=standard \
  IMAGE \
  tirn-probe-device connect ...
```

允许值为 `standard`、`desktop`，也接受完整名称。为保证 idle 多连接测试条件一致，`idle` 显式选择 standard
会直接报错退出。probe 启动后会打印实际 SDK variant、下载地址和 `TiRtcGetVersion()`。

可以在构建时覆盖 SDK 下载链接：

```sh
TIRTC_STANDARD_SDK_URL=https://.../standard.tgz \
TIRTC_DESKTOP_SDK_URL=https://.../desktop-standard.tgz \
PROBE_IMAGE=tirn-probe-device-runner:test \
./script/build_tirn_probe_runner_image.sh
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
