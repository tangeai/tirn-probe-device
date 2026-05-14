#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
owner='tangeai'
repo_name='tirtc-example-device'
latest_release_url="https://github.com/$owner/$repo_name/releases/latest"

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf '[demo] missing required command: %s\n' "$1" >&2
    exit 1
  fi
}

require_file() {
  if [ ! -f "$1" ]; then
    printf '[demo] missing required file: %s\n' "$1" >&2
    exit 1
  fi
}

print_usage() {
  cat <<'USAGE'
Usage: ./script/prepare.sh

Behavior:
  1. Resolve the latest GitHub release for tangeai/tirtc-example-device
  2. Download <tag>.zip from that release
  3. Replace local assets/ and 3rd/ with the downloaded runtime package
USAGE
}

case "${1:-}" in
  --help)
    print_usage
    exit 0
    ;;
  '')
    ;;
  *)
    printf '[demo] unknown argument: %s\n' "$1" >&2
    exit 1
    ;;
esac

require_command curl
require_command unzip
require_file "$repo_root/README.md"
require_file "$repo_root/Makefile"

work_dir=$(mktemp -d)
archive_path="$work_dir/runtime.zip"
cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM

printf '%s\n' '[demo] resolving latest runtime release'
release_url=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$latest_release_url")
release_tag=${release_url##*/}
if [ -z "$release_tag" ] || [ "$release_tag" = 'latest' ]; then
  printf '[demo] failed to resolve latest release tag from %s\n' "$release_url" >&2
  exit 1
fi
asset_name="$release_tag.zip"
asset_url="https://github.com/$owner/$repo_name/releases/download/$release_tag/$asset_name"
extract_root="$work_dir/extract"

printf '[demo] downloading %s\n' "$asset_url"
curl -fL "$asset_url" -o "$archive_path"

mkdir -p "$extract_root"
unzip -q "$archive_path" -d "$extract_root"
package_root=$(find "$extract_root" -mindepth 1 -maxdepth 1 -type d | head -n 1)
if [ -z "$package_root" ]; then
  printf '%s\n' '[demo] failed to locate extracted runtime package root' >&2
  exit 1
fi

rm -rf "$repo_root/assets" "$repo_root/3rd"
cp -R "$package_root/assets" "$repo_root/assets"
cp -R "$package_root/3rd" "$repo_root/3rd"

require_file "$repo_root/assets/audio.g711a"
require_file "$repo_root/assets/video.h264"
require_file "$repo_root/3rd/include/tiRTC.h"
require_file "$repo_root/3rd/include/basedef.h"
require_file "$repo_root/3rd/lib/libtirtc.a"

printf '[demo] prepared runtime from release %s\n' "$release_tag"
printf '[demo] assets ready: %s\n' "$repo_root/assets"
printf '[demo] 3rd ready: %s\n' "$repo_root/3rd"
