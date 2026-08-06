#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/botfarms-host-tests.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT
SANITIZERS="${BOTFARMS_HOST_TEST_SANITIZERS:-OFF}"

cmake -S "$ROOT_DIR/tests" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBOTFARMS_HOST_TEST_SANITIZERS="$SANITIZERS"
cmake --build "$BUILD_DIR" --parallel
if [[ "$SANITIZERS" == "ON" ]]; then
    ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_leaks=0:halt_on_error=1" \
        UBSAN_OPTIONS="${UBSAN_OPTIONS:+${UBSAN_OPTIONS}:}halt_on_error=1:print_stacktrace=1" \
        ctest --test-dir "$BUILD_DIR" --output-on-failure
else
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi
