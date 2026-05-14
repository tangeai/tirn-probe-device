# Linux 设备端送流 Demo

这是一个最小 Linux 设备端参考 Demo，主阅读入口是 `src/main.c`。

它只演示这一条固定流程：

`启动 -> 等待客户端连接 -> 立即送固定音视频 -> 断开后继续等待`

## 快速开始

### Docker 路径（推荐给 macOS / Windows / 不想污染宿主机的用户）

要求：

- Docker Desktop 或其他可用的 Docker Engine
- `docker` CLI 已在 PATH 中可用
- Windows 用户建议从 Git Bash 或 WSL 运行这些脚本

执行顺序固定为：

```sh
./script/prepare_in_docker.sh
./script/build_in_docker.sh
./script/run_demo_in_docker.sh \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key
```

例如：

```sh
./script/run_demo_in_docker.sh \
  --device-id TESTFENGABCD \
  --device-secret-key PRoGgX6lDelY4y9df2WW9U9NR10rBfXX
```

说明：

- 这些脚本会自动基于仓库内 `Dockerfile` 构建本地镜像 `tirtc-device-example-env:latest`
- `run_demo_in_docker.sh` 会在容器内重新编译 demo，确保实际运行的是当前源码
- Demo 启动时统一把 Nano 发送缓冲上限设置为 `1 MiB`
- 如果同一个 `device_id` 之前残留了旧容器，`run_demo_in_docker.sh` 会先清理旧容器，避免多进程污染
- 仓库不内置 Docker App；用户仍需自己安装并启动 Docker Desktop 或同类运行时
- 如不传 `--endpoint`，默认沿用 Nano upstream 内置的 `https://rtc.tange365.com`

### Native Linux x86_64 路径

要求：

- Linux x86_64
- `curl`
- `unzip`
- `python3`
- `gcc`
- `make`

执行顺序固定为：

```sh
./script/prepare.sh
./script/build.sh
./script/run_demo.sh \
  --device-id your_device_id \
  --device-secret-key your_device_secret_key
```

## 脚本分别做什么

### `./script/prepare.sh`

作用：

1. 查询 `tangeai/tirtc-example-device` 的 latest release
2. 下载对应的 `<tag>.zip`
3. 回填当前仓库的 `assets/` 和 `3rd/`
4. 校验下面这些文件已经就位：
   - `assets/audio.g711a`
   - `assets/video.h264`
   - `3rd/include/tiRTC.h`
   - `3rd/include/basedef.h`
   - `3rd/lib/libtirtc.a`

查看说明：

```sh
./script/prepare.sh --help
```

### `./script/build.sh`

作用：编译生成 Demo 可执行文件。

输出：

```text
build/linux_device_uplink_demo
```

如果缺少下面任一文件，`build.sh` 会直接失败：

- `3rd/include/tiRTC.h`
- `3rd/include/basedef.h`
- `3rd/lib/libtirtc.a`

### `./script/run_demo.sh`

作用：按给定 `device_id`、`device_secret_key` 拉起设备端送流 Demo；`endpoint` 可选。

如果缺少下面任一文件，`run_demo.sh` 会直接失败：

- `assets/audio.g711a`
- `assets/video.h264`
- `3rd/lib/libtirtc.a`
- `build/linux_device_uplink_demo`

### `./script/build_docker_image.sh`

作用：构建 Docker 路径共用的本地镜像。

默认镜像名：

```text
tirtc-device-example-env:latest
```

常用命令：

```sh
./script/build_docker_image.sh
./script/build_docker_image.sh --rebuild
```

### `./script/prepare_in_docker.sh`

作用：在 Docker 环境中执行 `./script/prepare.sh`。

适用场景：

- 宿主机不是 Linux x86_64
- 不想在宿主机手动安装 `curl`、`unzip`、`python3`

### `./script/build_in_docker.sh`

作用：在 Docker 环境中执行 `./script/build.sh`。

适用场景：

- 宿主机不是 Linux x86_64
- 想固定用仓库内 Docker 基线编译 demo

### `./script/run_demo_in_docker.sh`

作用：在 Docker 环境中串行执行 `./script/build.sh` 和 `./script/run_demo.sh`。

它还会额外做两件事：

1. 运行前自动清理同一个 `device_id` 的旧 demo 容器
2. 始终使用当前源码重新编译，避免误跑旧二进制
3. 不传 `--endpoint` 时沿用 Nano upstream 的默认 service endpoint

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
├── Dockerfile
├── 3rd/
│   ├── include/
│   │   ├── basedef.h
│   │   └── tiRTC.h
│   └── lib/
│       └── libtirtc.a
├── assets/
│   ├── audio.g711a
│   └── video.h264
├── script/
│   ├── build_docker_image.sh
│   ├── build.sh
│   ├── build_in_docker.sh
│   ├── docker_common.sh
│   ├── prepare.sh
│   ├── prepare_in_docker.sh
│   ├── run_demo.sh
│   └── run_demo_in_docker.sh
├── src/
│   ├── device_demo_streamer.c
│   ├── device_demo_streamer.h
│   └── main.c
├── Makefile
└── README.md
```

各目录职责：

- `src/main.c`：进程入口、参数解析、TiRTC 生命周期、连接管理
- `src/device_demo_streamer.c/.h`：固定音视频文件读取、切片、循环送流
- `Dockerfile`：Docker 路径的统一 Linux amd64 运行环境
- `script/build_docker_image.sh`：构建 Docker 路径共用镜像
- `script/prepare.sh`：下载 latest release 运行时包并回填 `assets/`、`3rd/`
- `script/prepare_in_docker.sh`：在 Docker 中执行运行时准备
- `script/build.sh`：编译入口
- `script/build_in_docker.sh`：在 Docker 中执行编译
- `script/run_demo.sh`：运行入口
- `script/run_demo_in_docker.sh`：在 Docker 中编译并运行 demo
- `script/docker_common.sh`：Docker 路径共用的镜像、挂载和容器清理逻辑
- `assets/`：固定媒体文件
- `3rd/`：TiRTC Nano 头文件和静态库
