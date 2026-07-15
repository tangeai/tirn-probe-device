#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test}
binary="$repo_root/build/linux-x86_64/tirtc_accel_device_probe"
revision=$(git -C "$repo_root" rev-parse HEAD)
source=$(git -C "$repo_root" remote get-url origin)
created=${BUILD_CREATED:-$(date -u '+%Y-%m-%dT%H:%M:%SZ')}
version=${IMAGE_VERSION:-${image##*:}}

if [ ! -x "$binary" ]; then
  printf '[probe-image] missing executable: %s\n' "$binary" >&2
  printf '%s\n' '[probe-image] build linux-x86_64 first, then rebuild this image.' >&2
  exit 1
fi

cd "$repo_root"
docker build \
  --platform linux/amd64 \
  --label "org.opencontainers.image.created=$created" \
  --label "org.opencontainers.image.revision=$revision" \
  --label "org.opencontainers.image.source=$source" \
  --label "org.opencontainers.image.title=tirtc-accel-probe-runner" \
  --label "org.opencontainers.image.version=$version" \
  -t "$image" \
  -f docker/probe-runner/Dockerfile \
  .
printf '[probe-image] built image: %s\n' "$image"
printf '[probe-image] source revision: %s\n' "$revision"
