#!/usr/bin/env bash
set -euo pipefail

qmake="${QMAKE:-qmake6}"
make_tool="${MAKE:-make}"
compiler="${CXX:-g++}"
configuration="both"
variant="all"
suite="all"
jobs="${JOBS:-2}"
keep_build=false
build_launcher=false

readonly suites=(buffer_pool chunk fifo fifo_view latest pool pool_view queue typed_pool)

usage() {
    cat <<'EOF'
Usage: run_h7_matrix.sh [options]

Options:
  --qmake <path>                     qmake executable (default: qmake6 or $QMAKE)
  --make <path>                      make executable (default: make or $MAKE)
  --compiler <path>                  C++ compiler used by qmake (default: g++ or $CXX)
  --configuration <debug|release|both>
  --variant <shadow_off|shadow_on|shadow_heur|cxx20_span|all>
  --suite <suite-name|all>            Default: all suites
  --jobs <count>                      Default: 2 or $JOBS
  --build-launcher                     Also build the dashboard from the clean directory
  --keep-build                        Keep the owned temporary build directory

Each build is generated under one fresh temporary directory. No source-tree
build, MOC, object, or binary artifact is read or removed by this script.
EOF
}

while (($#)); do
    case "$1" in
        --qmake) qmake="$2"; shift 2 ;;
        --make) make_tool="$2"; shift 2 ;;
        --compiler) compiler="$2"; shift 2 ;;
        --configuration) configuration="$2"; shift 2 ;;
        --variant) variant="$2"; shift 2 ;;
        --suite) suite="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        --build-launcher) build_launcher=true; shift ;;
        --keep-build) keep_build=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$configuration" in debug|release|both) ;; *) echo "Invalid configuration: $configuration" >&2; exit 2 ;; esac
case "$variant" in shadow_off|shadow_on|shadow_heur|cxx20_span|all) ;; *) echo "Invalid variant: $variant" >&2; exit 2 ;; esac
case "$suite" in all|buffer_pool|chunk|fifo|fifo_view|latest|pool|pool_view|queue|typed_pool) ;; *) echo "Invalid suite: $suite" >&2; exit 2 ;; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid jobs count: $jobs" >&2; exit 2; }

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
temp_root="$(cd -- "${TMPDIR:-/tmp}" && pwd)"
work_dir="$(mktemp -d "$temp_root/spsc-h7.XXXXXXXX")"

cleanup() {
    if "$keep_build"; then
        echo "Kept H7 matrix build directory: $work_dir"
        return
    fi
    case "$work_dir" in
        "$temp_root"/spsc-h7.*) rm -rf -- "$work_dir" ;;
        *) echo "Refusing to remove unexpected directory: $work_dir" >&2; return 1 ;;
    esac
}
trap cleanup EXIT

command -v "$qmake" >/dev/null
command -v "$make_tool" >/dev/null
command -v "$compiler" >/dev/null

if [[ "$configuration" == "both" ]]; then
    configurations=(debug release)
else
    configurations=("$configuration")
fi
if [[ "$variant" == "all" ]]; then
    variants=(shadow_off shadow_on shadow_heur cxx20_span)
else
    variants=("$variant")
fi
if "$build_launcher"; then
    variants+=(launcher)
fi
if [[ "$suite" == "all" ]]; then
    selected_suites=("${suites[@]}")
else
    selected_suites=("$suite")
fi

echo "[H7 matrix] compiler: $($compiler --version | head -n 1)"
echo "[H7 matrix] qmake: $($qmake -v | tail -n 1)"

for config in "${configurations[@]}"; do
    other_config=release
    [[ "$config" == "release" ]] && other_config=debug

    for target_variant in "${variants[@]}"; do
        if [[ "$target_variant" == "launcher" ]]; then
            project_file="$repo_root/qmake/launcher.pro"
            target_name=spsc_launcher
        else
            project_file="$repo_root/qmake/test_${target_variant}.pro"
            target_name="spsc_test_${target_variant}"
        fi
        [[ -f "$project_file" ]] || { echo "Missing qmake target: $project_file" >&2; exit 1; }

        build_dir="$work_dir/${config}-${target_variant}"
        qmake_dir="$build_dir/qmake"
        mkdir -p "$qmake_dir"

        echo "[H7 matrix] build config=$config variant=$target_variant"
        pushd "$qmake_dir" >/dev/null
        "$qmake" -o Makefile "$project_file" \
            "CONFIG+=$config" "CONFIG-=$other_config" \
            "QMAKE_CXX=$compiler" "QMAKE_LINK=$compiler"
        # The early MOC anchor in fifo_view_test.cpp makes Qt 6.4 qmake
        # discover that large source; materialize source-MOC output before
        # its consumers start compiling in a pristine tree.
        "$make_tool" -f Makefile mocables
        "$make_tool" -f Makefile -j"$jobs"
        popd >/dev/null

        executable="$build_dir/bin/$config/$target_name"
        [[ -x "$executable" ]] || { echo "Missing runner: $executable" >&2; exit 1; }

        if [[ "$target_variant" == "launcher" ]]; then
            echo "[H7 matrix] built config=$config variant=launcher"
            continue
        fi

        for suite_name in "${selected_suites[@]}"; do
            echo "[H7 matrix] run config=$config variant=$target_variant suite=$suite_name"
            "$executable" --run-suite "$suite_name"
        done
    done
done

echo "PASS: H7 qmake matrix"
