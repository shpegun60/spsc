#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-g++}"
source_file="tests/standalone/bounded_stack_probe.cpp"
limit_bytes=512
work_dir=""
extra_flags=()

usage() {
  cat <<'EOF'
Usage: check_bounded_stack_usage.sh [options]

Options:
  --compiler <path>   C++ compiler (default: g++ or $CXX)
  --source <path>     Probe source (default: tests/standalone/bounded_stack_probe.cpp)
  --limit <bytes>     Maximum permitted static frame (default: 512)
  --work-dir <path>   Keep artifacts in this directory
  --flag <flag>       Additional compiler flag; may be repeated
EOF
}

while (($# != 0)); do
  case "$1" in
    --compiler)
      compiler="$2"
      shift 2
      ;;
    --source)
      source_file="$2"
      shift 2
      ;;
    --limit)
      limit_bytes="$2"
      shift 2
      ;;
    --work-dir)
      work_dir="$2"
      shift 2
      ;;
    --flag)
      extra_flags+=("$2")
      shift 2
      ;;
    --flag=*)
      extra_flags+=("${1#--flag=}")
      shift
      ;;
    -h|--help)
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

if [[ ! "$limit_bytes" =~ ^[0-9]+$ ]] || ((limit_bytes == 0)); then
  echo "--limit must be a positive integer" >&2
  exit 2
fi

cleanup=false
if [[ -z "$work_dir" ]]; then
  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/spsc-bounded-stack.XXXXXX")"
  cleanup=true
else
  mkdir -p "$work_dir"
fi

if [[ "$cleanup" == true ]]; then
  trap 'rm -rf -- "$work_dir"' EXIT
fi

object_file="$work_dir/bounded_stack_probe.o"
usage_file="$work_dir/bounded_stack_probe.su"
rm -f -- "$object_file" "$usage_file"

"$compiler" \
  -std=c++17 -O2 -DNDEBUG \
  -Wall -Wextra -Werror -pedantic-errors \
  -fstack-usage -I. -Isrc/spsc \
  "${extra_flags[@]}" \
  -c "$source_file" -o "$object_file"

if [[ ! -s "$usage_file" ]]; then
  echo "Compiler did not produce stack-usage evidence: $usage_file" >&2
  exit 1
fi

entry_count=0
max_bytes=0
max_function=""
failed=false

while IFS=$'\t' read -r function bytes classification; do
  [[ "$bytes" =~ ^[0-9]+$ ]] || continue
  ((entry_count += 1))

  if ((bytes > max_bytes)); then
    max_bytes=$bytes
    max_function=$function
  fi

  if [[ "$classification" != "static" ]]; then
    echo "Unbounded/dynamic stack classification: $function ($classification)" >&2
    failed=true
  fi
  if ((bytes > limit_bytes)); then
    echo "Stack frame exceeds ${limit_bytes} bytes: ${bytes} bytes: $function" >&2
    failed=true
  fi
done < "$usage_file"

if ((entry_count == 0)); then
  echo "No parseable stack-usage entries in $usage_file" >&2
  exit 1
fi

printf 'Bounded stack gate: %s entries; maximum %s bytes; limit %s bytes\n' \
  "$entry_count" "$max_bytes" "$limit_bytes"
printf 'Maximum frame: %s\n' "$max_function"

if [[ "$failed" == true ]]; then
  exit 1
fi
