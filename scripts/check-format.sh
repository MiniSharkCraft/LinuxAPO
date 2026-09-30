#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: %s <base-revision>\n' "$0" >&2
  printf 'Checks changed, tracked SkyAPO C++ files against clang-format.\n' >&2
}

if [[ $# -ne 1 ]]; then
  usage
  exit 2
fi

base_revision=$1
if ! git rev-parse --verify --quiet "${base_revision}^{commit}" >/dev/null; then
  printf 'check-format: base revision is not available: %s\n' "$base_revision" >&2
  exit 2
fi

formatter=${CLANG_FORMAT:-clang-format}
if ! command -v "$formatter" >/dev/null 2>&1; then
  printf 'check-format: clang-format not found (set CLANG_FORMAT to override)\n' >&2
  exit 2
fi

merge_base=$(git merge-base "$base_revision" HEAD)
mapfile -t files < <(
  git diff --name-only --diff-filter=ACMR "$merge_base" HEAD -- src tests |
    awk '
      /\.(cc|cpp|cxx|h|hh|hpp|hxx)$/ && $0 !~ /^src\/ui\/upstream\// { print }
    '
)

if ((${#files[@]} == 0)); then
  printf 'clang-format: no changed SkyAPO C++ files to check\n'
  exit 0
fi

printf 'Checking %d changed SkyAPO C++ file(s) with %s\n' "${#files[@]}" "$formatter"
"$formatter" --dry-run --Werror "${files[@]}"
