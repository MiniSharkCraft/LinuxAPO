#!/usr/bin/env bash
set -euo pipefail

if (( $# < 2 )); then
  printf 'usage: %s OUTPUT_DIR ARTIFACT [ARTIFACT ...]\n' "$0" >&2
  exit 2
fi

output_dir=$1
shift
mkdir -p -- "$output_dir"
output_dir=$(realpath -- "$output_dir")
sums_path="$output_dir/SHA256SUMS"
if [[ -e "$sums_path" ]]; then
  printf 'error: refusing to overwrite existing checksum file: %s\n' "$sums_path" >&2
  exit 2
fi

resolved=()
for artifact in "$@"; do
  if [[ ! -f "$artifact" ]]; then
    printf 'error: release artifact is missing: %s\n' "$artifact" >&2
    exit 2
  fi
  relative_path=$(realpath --relative-to="$output_dir" "$artifact")
  if [[ "$relative_path" == ".." || "$relative_path" == ../* ]]; then
    printf 'error: artifact must be inside the release directory: %s\n' "$artifact" >&2
    exit 2
  fi
  resolved+=("$relative_path")
done

temporary_sums=$(mktemp "$output_dir/.SHA256SUMS.XXXXXXXX")
trap 'rm -f -- "$temporary_sums"' EXIT
(
  cd -- "$output_dir"
  sha256sum -- "${resolved[@]}" > "$temporary_sums"
)
mv --no-clobber -- "$temporary_sums" "$sums_path"
printf 'created %s\n' "$sums_path"
