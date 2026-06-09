# TiRTC 设备端送流 Demo

这是一个最小设备端参考 Demo，主阅读入口是 `src/main.c`。

Demo 演示一条固定流程：

```text
启动 -> 等待客户端连接 -> 立即发送固定音视频 -> 断开后继续等待
```

仓库已经预置默认媒体文件和 TiRTC SDK。正常情况下，克隆后不需要再下载 release 包或 SDK 包。

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

媒体文件已经固化在仓库：

```text
assets/audio.g711a
assets/video.h264
```

默认 SDK 包和解包后的 SDK 都已经固化在仓库：

```text
3rd/packages/tirtc__macos-arm64__xcode26.5__v0.1.4.tgz
3rd/packages/tirtc__macos-arm64__xcode26.5__v0.1.4.tgz.sha256
3rd/packages/tirtc__linux-x86_64__gcc11-glibc2.35__v0.1.4.tgz
3rd/packages/tirtc__linux-x86_64__gcc11-glibc2.35__v0.1.4.tgz.sha256

3rd/macos-arm64/
3rd/linux-x86_64/
```

`script/build.sh` 会按当前宿主平台自动选择：

- macOS arm64 -> `3rd/macos-arm64`
- Linux x86_64 -> `3rd/linux-x86_64`

构建产物：

```text
build/macos-arm64/device_uplink_demo
build/macos-arm64/libtgrtc.dylib
build/linux-x86_64/device_uplink_demo
```

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

SDK 包浏览地址：

- macOS arm64: https://repo-sdk.tange-ai.com/service/rest/repository/browse/tirtc-sdks/releases/macos-arm64/
- Linux x86_64: https://repo-sdk.tange-ai.com/service/rest/repository/browse/tirtc-sdks/releases/linux-x86_64/

当前默认包：

```text
tirtc__macos-arm64__xcode26.5__v0.1.4.tgz
tirtc__linux-x86_64__gcc11-glibc2.35__v0.1.4.tgz
```

下载新包时，实际文件 URL 使用 `/repository/` 路径。例如：

```sh
cd 3rd/packages

curl -fLO https://repo-sdk.tange-ai.com/repository/tirtc-sdks/releases/macos-arm64/tirtc__macos-arm64__xcode26.5__v0.1.4.tgz
curl -fLO https://repo-sdk.tange-ai.com/repository/tirtc-sdks/releases/macos-arm64/tirtc__macos-arm64__xcode26.5__v0.1.4.tgz.sha256

curl -fLO https://repo-sdk.tange-ai.com/repository/tirtc-sdks/releases/linux-x86_64/tirtc__linux-x86_64__gcc11-glibc2.35__v0.1.4.tgz
curl -fLO https://repo-sdk.tange-ai.com/repository/tirtc-sdks/releases/linux-x86_64/tirtc__linux-x86_64__gcc11-glibc2.35__v0.1.4.tgz.sha256
```

解包到工程约定目录：

```sh
rm -rf 3rd/macos-arm64
mkdir -p 3rd/macos-arm64
tar -xzf 3rd/packages/tirtc__macos-arm64__xcode26.5__v0.1.4.tgz \
  -C 3rd/macos-arm64 \
  --strip-components 1

rm -rf 3rd/linux-x86_64
mkdir -p 3rd/linux-x86_64
tar -xzf 3rd/packages/tirtc__linux-x86_64__gcc11-glibc2.35__v0.1.4.tgz \
  -C 3rd/linux-x86_64 \
  --strip-components 1
```

`Makefile` 只依赖解包后的目录结构，不依赖 tgz 文件名。替换 SDK 时，保持 `3rd/macos-arm64` 和 `3rd/linux-x86_64` 的目录结构一致即可。

Linux 目录中还可能存在 `__nosctp` 包；当前 Demo 默认使用带 `libusrsctp.a` 的标准包。如果切换到 `__nosctp` 包，需要同步调整 `Makefile` 中 Linux SDK 的库名。

## macOS 上自选 Docker 跑 Linux Demo

本工程不再维护 Dockerfile 或 Docker 脚本。确实需要在 macOS 上跑 Linux x86_64 Demo 时，可以自己使用临时容器：

```sh
docker run --rm --platform linux/amd64 \
  -v "$PWD":/work \
  -w /work \
  ubuntu:22.04 \
  bash -lc 'apt-get update && apt-get install -y --no-install-recommends build-essential ca-certificates make && ./script/build.sh && ./script/run_demo.sh --device-id your_device_id --device-secret-key your_device_secret_key'
```

这只是使用者自选的 Linux 运行环境，不是项目维护入口。

## 运行后会发生什么

1. 校验启动参数和必需文件
2. 启动 TiRTC 设备端
3. 等待客户端连接
4. 客户端连接后立即开始送流
5. 首个视频发送优先输出 I 帧，尽快出图
6. 音视频固定循环送流
7. 客户端断开后停止当前连接送流
8. 进程继续等待下一次连接

## 目录结构

```text
.
├── 3rd/
│   ├── linux-x86_64/
│   ├── macos-arm64/
│   └── packages/
├── assets/
│   ├── audio.g711a
│   └── video.h264
├── script/
│   ├── build.sh
│   └── run_demo.sh
├── src/
│   ├── device_demo_streamer.c
│   ├── device_demo_streamer.h
│   └── main.c
├── Makefile
└── README.md
```
