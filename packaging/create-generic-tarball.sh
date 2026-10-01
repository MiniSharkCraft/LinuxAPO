#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$repo_dir/build"}
output_dir=${2:-"$repo_dir/release-artifacts"}
version=$(sed -n 's/^project(SkyAPO VERSION \([^ ]*\) LANGUAGES.*/\1/p' "$repo_dir/CMakeLists.txt")
architecture=$(uname -m)

if [[ "$architecture" != "x86_64" ]]; then
  printf 'error: generic release archive currently targets x86_64, got %s\n' "$architecture" >&2
  exit 2
fi
if [[ ! -f "$build_dir/cmake_install.cmake" ]]; then
  printf 'error: %s is not a configured CMake build directory\n' "$build_dir" >&2
  exit 2
fi
if [[ -z "$version" ]]; then
  printf 'error: could not read the CMake project version\n' >&2
  exit 2
fi

archive_name="skyapo-${version}-linux-x86_64.tar.zst"
mkdir -p -- "$output_dir"
archive_path="$output_dir/$archive_name"
if [[ -e "$archive_path" ]]; then
  printf 'error: refusing to overwrite existing release artifact: %s\n' "$archive_path" >&2
  exit 2
fi

stage_dir=$(mktemp -d "${TMPDIR:-/tmp}/skyapo-release-stage.XXXXXXXX")
archive_stage=$(mktemp -d "$output_dir/.skyapo-release-output.XXXXXXXX")
temporary_archive="$archive_stage/$archive_name"
cleanup() {
  rm -rf -- "$stage_dir"
  rm -rf -- "$archive_stage"
}
trap cleanup EXIT

payload_name="skyapo-${version}-linux-x86_64"
install_root="$stage_dir/$payload_name"
mkdir -p -- "$install_root"
DESTDIR="$install_root" cmake --install "$build_dir" --prefix /usr --strip
install -m 0644 "$repo_dir/packaging/TAR_INSTALL.txt" "$install_root/INSTALL.txt"

tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
  -C "$stage_dir" -cf - "$payload_name" |
  zstd -T0 -19 -q -o "$temporary_archive"
mv --no-clobber -- "$temporary_archive" "$archive_path"
printf 'created %s\n' "$archive_path"
