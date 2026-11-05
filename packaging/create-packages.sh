#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$(realpath -- "${1:-$repo_dir/build}")
output_dir=${2:-"$build_dir/packages"}
config="$build_dir/CPackConfig.cmake"
# Keep large CPack staging on disk by default. Callers may explicitly select
# another location with SKYAPO_TEMP_DIRECTORY; an inherited TMPDIR is often
# the host's small tmpfs and is therefore not a safe package-staging default.
temp_root=${SKYAPO_TEMP_DIRECTORY:-}
if [[ -z "$temp_root" && -f "$build_dir/CMakeCache.txt" ]]; then
  temp_root=$(sed -n \
    's/^SKYAPO_TEST_TEMP_DIRECTORY:PATH=//p' "$build_dir/CMakeCache.txt" | head -n 1)
fi
temp_root=${temp_root:-"$build_dir/tmp"}

if [[ ! -f "$config" ]]; then
  printf 'error: no CPack configuration at %s; configure the build first\n' "$config" >&2
  exit 2
fi
mkdir -p -- "$temp_root" "$output_dir"
output_dir=$(realpath -- "$output_dir")
stage_dir=$(mktemp -d "$temp_root/skyapo-cpack.XXXXXXXX")
success=0
cleanup() {
  local status=$?
  if ((success)); then
    rm -rf -- "$stage_dir"
  else
    printf 'CPack failed; temporary package work retained for diagnosis: %s\n' \
      "$stage_dir" >&2
  fi
  return "$status"
}
trap cleanup EXIT

if ! TMPDIR="$temp_root" cpack --config "$config" --verbose -B "$stage_dir"; then
  exit 1
fi

shopt -s nullglob
artifacts=("$stage_dir"/*.deb "$stage_dir"/*.rpm)
if ((${#artifacts[@]} == 0)); then
  printf 'error: CPack completed but produced no DEB or RPM artifacts\n' >&2
  exit 1
fi
for artifact in "${artifacts[@]}"; do
  destination="$output_dir/$(basename -- "$artifact")"
  if [[ -e "$destination" ]]; then
    printf 'error: refusing to overwrite existing package: %s\n' "$destination" >&2
    exit 1
  fi
done
for artifact in "${artifacts[@]}"; do
  mv -- "$artifact" "$output_dir/"
  printf 'created %s\n' "$output_dir/$(basename -- "$artifact")"
done
success=1
