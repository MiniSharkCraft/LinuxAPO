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
formatter_python=${CLANG_FORMAT_PYTHON:-python3}
required_formatter_version=18.1.3
if ! command -v "$formatter" >/dev/null 2>&1; then
  printf 'check-format: clang-format not found (set CLANG_FORMAT to override)\n' >&2
  exit 2
fi
formatter_version_output=$("$formatter" --version) || {
  printf 'check-format: failed to query clang-format version from %s\n' \
    "$formatter" >&2
  exit 2
}
formatter_version=$(sed -nE \
  's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
  <<<"$formatter_version_output" | head -n 1)
if [[ "$formatter_version" != "$required_formatter_version" ]]; then
  printf 'check-format: requires clang-format %s; found %s\n' \
    "$required_formatter_version" "${formatter_version:-unknown} ($formatter_version_output)" >&2
  printf 'Install the pinned version with: python3 -m pip install clang-format==%s\n' \
    "$required_formatter_version" >&2
  exit 2
fi

formatter_diff=${CLANG_FORMAT_DIFF:-}
if [[ -z "$formatter_diff" ]]; then
  for candidate in clang-format-diff clang-format-diff.py \
    /usr/share/clang/clang-format-diff.py; do
    if command -v "$candidate" >/dev/null 2>&1; then
      formatter_diff=$(command -v "$candidate")
      break
    elif [[ -x "$candidate" ]]; then
      formatter_diff=$candidate
      break
    fi
  done
fi
if [[ -z "$formatter_diff" ]]; then
  printf 'check-format: clang-format-diff not found\n' >&2
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

printf 'Checking changed lines in %d SkyAPO C++ file(s) with %s\n' \
  "${#files[@]}" "$formatter"
diff_output=""
formatter_status=0
if diff_output=$(git diff --no-color --unified=0 "$merge_base" HEAD -- src tests |
  "$formatter_python" "$formatter_diff" -p1 -style=file -fallback-style=none \
    -binary "$formatter"); then
  :
else
  formatter_status=$?
fi
if [[ -n "$diff_output" ]]; then
  printf '%s\n' "$diff_output"
  printf 'clang-format check failed; format the changed lines above.\n' >&2
  exit 1
fi
if ((formatter_status != 0)); then
  printf 'clang-format-diff failed with exit code %d without producing a diff.\n' \
    "$formatter_status" >&2
  exit "$formatter_status"
fi
