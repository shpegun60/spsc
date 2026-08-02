#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-clang++}"
sanitizer="address,undefined"
keep_build=false

usage() {
    cat <<'EOF'
Usage: run_h7_sanitizers.sh [options]

Options:
  --compiler <path>                  C++ compiler (default: clang++ or $CXX)
  --sanitizer <address,undefined|thread>
                                     Sanitizer set (default: address,undefined)
  --keep-build                       Keep the owned temporary build directory
EOF
}

while (($#)); do
    case "$1" in
        --compiler)
            compiler="$2"
            shift 2
            ;;
        --sanitizer)
            sanitizer="$2"
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

case "$sanitizer" in
    address,undefined|thread) ;;
    *)
        echo "Unsupported sanitizer set: $sanitizer" >&2
        exit 2
        ;;
esac

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source_path="$repo_root/tests/standalone/h7_atomic_observer_stress.cpp"
temp_root="$(cd -- "${TMPDIR:-/tmp}" && pwd)"
work_dir="$(mktemp -d "$temp_root/spsc-h7-sanitize.XXXXXXXX")"

cleanup() {
    if "$keep_build"; then
        echo "Kept H7 sanitizer build directory: $work_dir"
        return
    fi
    case "$work_dir" in
        "$temp_root"/spsc-h7-sanitize.*) rm -rf -- "$work_dir" ;;
        *) echo "Refusing to remove unexpected directory: $work_dir" >&2; return 1 ;;
    esac
}
trap cleanup EXIT

command -v "$compiler" >/dev/null

executable="$work_dir/h7_atomic_observer_stress"
compile_flags=(
    -std=c++17 -O1 -g -fno-omit-frame-pointer
    -Wall -Wextra -Werror -pedantic -pthread
    -DSPSC_ENABLE_SHADOW_INDICES=1
    -DSPSC_SHADOW_ALLOW_32BIT=0
    -fsanitize="$sanitizer"
    -I"$repo_root" -I"$repo_root/src/spsc"
)

echo "[H7 sanitizer] compiler: $($compiler --version | head -n 1)"
echo "[H7 sanitizer] flags: ${compile_flags[*]}"
"$compiler" "${compile_flags[@]}" "$source_path" -o "$executable"

if [[ "$sanitizer" == "thread" ]]; then
    TSAN_OPTIONS="halt_on_error=1" "$executable"
else
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "$executable"
fi

echo "PASS: H7 $sanitizer observer sanitizer smoke"
