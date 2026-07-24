#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirn-probe-device-runner:test}
revision=$(git -C "$repo_root" rev-parse HEAD)
source=$(git -C "$repo_root" remote get-url origin)
created=${BUILD_CREATED:-$(date -u '+%Y-%m-%dT%H:%M:%SZ')}
version=${IMAGE_VERSION:-${image##*:}}
standard_sdk_url=${TIRTC_STANDARD_SDK_URL:-https://repo-sdk.tange-ai.com/repository/tirtc-sdks/ci/linux-x86_64/tirtc__linux-x86_64__gcc11-glibc2.35__e84eff7__20260714-111114__standard/tirtc__linux-x86_64__gcc11-glibc2.35__e84eff7__20260714-111114__standard.tgz}
desktop_sdk_url=${TIRTC_DESKTOP_SDK_URL:-https://repo-sdk.tange-ai.com/repository/tirtc-sdks/ci/linux-x86_64-desktop/tirtc__linux-x86_64-desktop__webrtc-linux-clang__e84eff7__20260714-112702__standard/tirtc__linux-x86_64-desktop__webrtc-linux-clang__e84eff7__20260714-112702__standard.tgz}
builder_image=${PROBE_BUILDER_IMAGE:-${PROBE_BASE_IMAGE:-docker-hub.tange365.com/public/ubuntu:22.04}}
runtime_image=${PROBE_RUNTIME_IMAGE:-${PROBE_BASE_IMAGE:-docker-hub.tange365.com/public/ubuntu:22.04}}
install_packages=${PROBE_INSTALL_PACKAGES:-1}

cd "$repo_root"
docker build \
  --platform linux/amd64 \
  --build-arg "PROBE_BUILDER_IMAGE=$builder_image" \
  --build-arg "PROBE_RUNTIME_IMAGE=$runtime_image" \
  --build-arg "PROBE_INSTALL_PACKAGES=$install_packages" \
  --build-arg "TIRTC_STANDARD_SDK_URL=$standard_sdk_url" \
  --build-arg "TIRTC_DESKTOP_SDK_URL=$desktop_sdk_url" \
  --label "org.opencontainers.image.created=$created" \
  --label "org.opencontainers.image.revision=$revision" \
  --label "org.opencontainers.image.source=$source" \
  --label "org.opencontainers.image.title=tirn-probe-device-runner" \
  --label "org.opencontainers.image.version=$version" \
  --label "ai.tange.tirtc.standard-sdk.url=$standard_sdk_url" \
  --label "ai.tange.tirtc.desktop-sdk.url=$desktop_sdk_url" \
  -t "$image" \
  -f docker/probe-runner/Dockerfile \
  .
printf '[probe-image] built image: %s\n' "$image"
printf '[probe-image] source revision: %s\n' "$revision"
printf '[probe-image] TiRTC standard SDK: %s\n' "$standard_sdk_url"
printf '[probe-image] TiRTC desktop SDK: %s\n' "$desktop_sdk_url"
printf '[probe-image] builder image: %s\n' "$builder_image"
printf '[probe-image] runtime image: %s\n' "$runtime_image"
