#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-g++}"
keep_build=false

usage() {
    cat <<'EOF'
Usage: run_h6_32bit_shadow_matrix.sh [--compiler <path>] [--keep-build]

Builds and runs the H6 shadow-gate smoke test twice with a genuine -m32 target.
The host must have the compiler's 32-bit C++ runtime and development libraries.
EOF
}

while (($#)); do
    case "$1" in
        --compiler)
            compiler="$2"
            shift 2
            ;;
        --keep-build)
            keep_build=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source_path="$repo_root/tests/standalone/h6_32bit_shadow_matrix.cpp"
temp_root="$(cd -- "${TMPDIR:-/tmp}" && pwd)"
work_dir="$(mktemp -d "$temp_root/spsc-h6-32bit.XXXXXXXX")"

cleanup() {
    if "$keep_build"; then
        echo "Kept H6 32-bit build directory: $work_dir"
        return
    fi
    case "$work_dir" in
        "$temp_root"/spsc-h6-32bit.*) rm -rf -- "$work_dir" ;;
        *) echo "Refusing to remove unexpected directory: $work_dir" >&2; return 1 ;;
    esac
}
trap cleanup EXIT

command -v "$compiler" >/dev/null

for allow_32bit in 0 1; do
    executable="$work_dir/shadow_allow_32bit_$allow_32bit"
    "$compiler" \
        -m32 -std=c++17 -Wall -Wextra -Werror -pedantic \
        -I"$repo_root" -I"$repo_root/src/spsc" \
        -DSPSC_ENABLE_SHADOW_INDICES=1 \
        -DSPSC_SHADOW_ALLOW_32BIT="$allow_32bit" \
        "$source_path" -pthread -o "$executable"
    "$executable"
    echo "PASS: genuine 32-bit shadow matrix allow_32bit=$allow_32bit"
done
