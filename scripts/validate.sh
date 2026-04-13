#!/bin/bash
# Manual full-suite validation on the current POSIX platform (Linux, macOS, …).
#
# Runs the same flow in all cases: unit tests, stress tests (single pass
# plus a shuffled repeat to probe intermittent bugs), and crash-recovery
# tests.  Use before tagging a release, after touching lock-free or
# platform-specific code, or any time you want more confidence than CI
# gives you.
#
# Why it's especially valuable on a mac (without being mac-specific):
#   - Apple Silicon is ARM64 with weaker memory ordering than x86's TSO.
#     The shuffled stress repeat here exercises acquire/release pairings
#     that x86 hides.  GitHub's macOS runners are a paid feature, so
#     this script is how you stand in for that CI job.
#   - Darwin's __ulock_wait/__ulock_wake futex is a distinct code path
#     from Linux's SYS_futex; blocking receive/timeout under churn and
#     the crash-recovery paths are where this matters.
#   - The ABI static_asserts (sizeof(Header), sizeof(SchemaInfo)) are
#     compile-time checks that must hold on the real target toolchain.
# None of those reasons make the script Linux-hostile — they just
# highlight where it earns its keep.  On x86 Linux it still catches
# regressions that a quick unit-test run would miss.
#
# Usage: scripts/validate.sh [build_dir]
#
# Defaults: build_dir = build_validate

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

source "$SCRIPT_DIR/lib/log.sh"

BUILD_DIR="${1:-build_validate}"
BUILD_PATH="$PROJECT_DIR/$BUILD_DIR"

info "Platform: $(uname -s) $(uname -m)"

step "Installing Conan dependencies → $BUILD_DIR"
cd "$PROJECT_DIR"
conan install conanfile.py -of="$BUILD_DIR" --build=missing -o unit_tests=True

step "Configuring CMake (Release, unit + crash tests)"
cmake -S "$PROJECT_DIR" -B "$BUILD_PATH" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$BUILD_PATH" \
    -DBUILD_UNIT_TESTS=ON

step "Building"
cmake --build "$BUILD_PATH" -j

step "Unit tests (ABI asserts, schema protocol, diff, hash)"
"$BUILD_PATH/kickmsg_unit"
success "Unit tests passed"

step "Stress tests — single pass"
"$BUILD_PATH/kickmsg_stress_test"
success "Stress tests passed"

# Repeat the stress suite several times with shuffled order.  The whole
# point of running on ARM64 is that memory-ordering bugs appear
# intermittently — a single run that happens to pass tells us nothing.
# Ten passes with shuffled ordering is the 80/20 of catching real races
# without blowing out the wall-clock budget.
step "Stress tests — shuffled repeat x10 (ARM64 memory-ordering probe)"
"$BUILD_PATH/kickmsg_stress_test" --gtest_repeat=10 --gtest_shuffle
success "Shuffled stress repeat passed"

step "Crash / recovery tests"
"$BUILD_PATH/kickmsg_crash_test"
success "Crash tests passed"

echo
success "macOS validation complete — $BUILD_DIR clean."
echo
info "Optional next steps:"
info "  - Rebuild with -DENABLE_TSAN=ON and rerun the stress suite to"
info "    catch data races.  Darwin futex paths may produce some TSAN"
info "    noise, but real races in the ring / Treiber / schema code"
info "    still surface."
info "  - Run with --gtest_repeat=100 (instead of 10) before tagging a"
info "    release, if you want extra confidence."
