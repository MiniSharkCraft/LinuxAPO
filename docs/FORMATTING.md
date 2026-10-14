# C++ formatting

The repository pins `clang-format` **18.1.3**. The GitHub Actions workflow and
`scripts/check-format.sh` both enforce this exact version; use it locally so the
result matches CI.

On Linux, create a small cached virtual environment and install the matching
PyPI package (it supplies `clang-format`, `clang-format-diff.py`, and the Python
module the diff helper imports):

```sh
format_venv="${XDG_CACHE_HOME:-$HOME/.cache}/linuxapo/clang-format-18.1.3"
mkdir -p "$(dirname "$format_venv")"
python3 -m venv "$format_venv"
"$format_venv/bin/python" -m pip install clang-format==18.1.3

export CLANG_FORMAT="$format_venv/bin/clang-format"
export CLANG_FORMAT_DIFF="$format_venv/bin/clang-format-diff.py"
export CLANG_FORMAT_PYTHON="$format_venv/bin/python"
scripts/check-format.sh origin/main
```

Pass the target branch's base revision when checking a pull request. The script
prints the file and unified formatting diff for violations, while distinguishing
a formatting mismatch from a formatter/helper execution error. It only checks
changed C++ lines; `.clang-format` is the authoritative style configuration.
