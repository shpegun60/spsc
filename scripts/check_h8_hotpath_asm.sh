#!/usr/bin/env bash
# H8 assembly regression check for the public single-item FIFO and queue probes.
#
# `try_push()` can carry one producer-owned head snapshot to its release
# publication, so the generated function must load head exactly once. The
# public consumer probe intentionally consists of `try_front()` followed by
# `pop()`: without changing that public contract it has one tail snapshot per
# operation, hence exactly two tail loads are expected.

set -euo pipefail

compiler="g++"

while (($# > 0)); do
    case "$1" in
        --compiler)
            (($# >= 2)) || { echo "missing value for --compiler" >&2; exit 2; }
            compiler="$2"
            shift 2
            ;;
        *)
            echo "usage: $0 [--compiler CXX]" >&2
            exit 2
            ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/spsc-h8-asm.XXXXXX")"
assembly="$work_dir/hotpath.s"

cleanup() {
    rm -rf "$work_dir"
}
trap cleanup EXIT

"$compiler" \
    -std=c++17 -O3 -DNDEBUG -masm=intel \
    -DSPSC_ENABLE_SHADOW_INDICES=1 -DSPSC_SHADOW_ALLOW_32BIT=0 \
    -I"$repo_root" \
    -I"$repo_root/src" \
    -I"$repo_root/third_party/rigtorp_spscqueue/include" \
    -S "$repo_root/benchmarks/hotpath_probe.cpp" \
    -o "$assembly"

function_body() {
    local symbol="$1"
    awk -v symbol="$symbol" '
        # GCC emits a bare label, while Clang appends a source-symbol comment
        # (`symbol: # @symbol`).  Match the label prefix rather than requiring
        # the complete line to be exactly the label.
        $0 ~ ("^[[:space:]]*" symbol ":") { in_body = 1 }
        in_body { print }
        in_body && $0 ~ /^[[:space:]]*\.size[[:space:]]/ { exit }
        in_body && $0 ~ /^[[:space:]]*\.seh_endproc/ { exit }
    ' "$assembly"
}

require_count() {
    local expected="$1"
    local actual="$2"
    local description="$3"
    local body="$4"

    if [[ "$actual" != "$expected" ]]; then
        echo "H8 assembly check failed: expected $expected $description, got $actual" >&2
        echo "--- probe body ---" >&2
        printf '%s\n' "$body" >&2
        exit 1
    fi
}

producer_body="$(function_body spsc_fifo_producer)"
consumer_body="$(function_body spsc_fifo_consumer)"
queue_producer_body="$(function_body spsc_queue_producer)"
queue_consumer_body="$(function_body spsc_queue_consumer)"

if [[ -z "$producer_body" || -z "$consumer_body" ||
      -z "$queue_producer_body" || -z "$queue_consumer_body" ]]; then
    echo "H8 assembly check failed: expected probe symbols were not emitted" >&2
    exit 1
fi

# Both GCC and Clang use Intel syntax here.  A source operand with no offset
# is FIFO's producer head at offset 0.  The FIFO payload starts at offset 256,
# so no payload load matches this expression.
producer_head_loads="$(printf '%s\n' "$producer_body" | grep -Eic \
    '^[[:space:]]*mov[a-z]*[[:space:]]+[[:alnum:]]+,[[:space:]]+(qword ptr)[[:space:]]*\[[[:alnum:]]+\]' || true)"

# CFA's consumer-owned tail is at offset 128. GCC emits `128[reg]`, while
# Clang emits `[reg + 128]`. Count a source MOV and a memory increment as
# owner reads: Clang may fold pop's final owner read/commit into an in-place
# `inc qword ptr [...]`, whereas GCC materializes it as a separate MOV.
# The public front()+pop() probe therefore has two tail read/commit accesses.
consumer_tail_reads="$(printf '%s\n' "$consumer_body" | grep -Eic \
    '^[[:space:]]*((mov[a-z]*[[:space:]]+[[:alnum:]]+,[[:space:]]+(qword ptr)[[:space:]]*(128\[[[:alnum:]]+\]|\[[[:alnum:]]+[[:space:]]*\+[[:space:]]*128\]))|((inc|add)[a-z]*[[:space:]]+(qword ptr)[[:space:]]*(128\[[[:alnum:]]+\]|\[[[:alnum:]]+[[:space:]]*\+[[:space:]]*128\])))' || true)"

# queue_base contributes the allocation-state byte before the cache-line-aligned
# SPSCbase. In this probe queue's head is therefore at offset 64 and its tail at
# offset 192. As with FIFO, count only owner reads/read-commits, never the final
# producer publication store.
queue_producer_head_loads="$(printf '%s\n' "$queue_producer_body" | grep -Eic \
    '^[[:space:]]*mov[a-z]*[[:space:]]+[[:alnum:]]+,[[:space:]]+(qword ptr)[[:space:]]*(64\[[[:alnum:]]+\]|\[[[:alnum:]]+[[:space:]]*\+[[:space:]]*64\])' || true)"

queue_consumer_tail_reads="$(printf '%s\n' "$queue_consumer_body" | grep -Eic \
    '^[[:space:]]*((mov[a-z]*[[:space:]]+[[:alnum:]]+,[[:space:]]+(qword ptr)[[:space:]]*(192\[[[:alnum:]]+\]|\[[[:alnum:]]+[[:space:]]*\+[[:space:]]*192\]))|((inc|add)[a-z]*[[:space:]]+(qword ptr)[[:space:]]*(192\[[[:alnum:]]+\]|\[[[:alnum:]]+[[:space:]]*\+[[:space:]]*192\])))' || true)"

require_count 1 "$producer_head_loads" "producer head load" "$producer_body"
require_count 2 "$consumer_tail_reads" "consumer tail read/commit accesses" "$consumer_body"
require_count 1 "$queue_producer_head_loads" "queue producer head load" "$queue_producer_body"
require_count 2 "$queue_consumer_tail_reads" "queue consumer tail read/commit accesses" "$queue_consumer_body"

echo "PASS: H8 hot-path assembly ($compiler): fifo=1/2 queue=1/2 owner accesses"
