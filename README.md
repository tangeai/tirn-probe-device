# TiRTC 设备端送流 Demo

这是一个设备端本地媒体循环送流 Demo：启动后等待客户端连接，连接后循环发送 `assets/audio.g711a` 和 `assets/video.h264`。

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

`Makefile` 只依赖解包后的目录结构，不依赖 tgz 文件名。Linux 标准包和 `__nosctp` 包都可直接使用。

## macOS 上自选 Docker 跑 Linux Demo

需要在 macOS 上临时跑 Linux x86_64 版时：

```sh
docker run --rm --platform linux/amd64 \
  -v "$PWD":/work \
  -w /work \
  ubuntu:22.04 \
  bash -lc 'apt-get update && apt-get install -y --no-install-recommends build-essential ca-certificates make && ./script/build.sh && ./script/run_demo.sh --device-id your_device_id --device-secret-key your_device_secret_key'
```
