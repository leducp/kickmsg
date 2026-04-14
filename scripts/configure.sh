#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

source "$SCRIPT_DIR/lib/log.sh"
source "$SCRIPT_DIR/lib/build_options.sh"

usage() {
    cat <<EOF
Usage: $0 <build_dir> [OPTIONS]

Manage kickmsg build options. Options are stored in <build_dir>/.buildconfig
and consumed by setup_build.sh (Conan + CMake).

Arguments:
  build_dir                 Build directory (created if it does not exist)

Options:
  --with=<feature>        Enable a feature (use 'all' for every feature)
  --without=<feature>     Disable a feature (use 'all' for every feature)
  --show                  Show current configuration and exit
  -h, --help              Show this help message and exit

Features: ${OPT_NAMES[*]}

Examples:
  $0 build --with=unit_tests
  $0 build --with=unit_tests --with=benchmarks
  $0 build --with=all
EOF
}

build_dir=""
ACTIONS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --with=*)
            ACTIONS+=("with:${1#--with=}")
            shift
            ;;
        --without=*)
            ACTIONS+=("without:${1#--without=}")
            shift
            ;;
        --show)
            ACTIONS+=("show")
            shift
            ;;
        *)
            if [ -z "$build_dir" ]; then
                build_dir="$1"
            else
                error "Unknown argument: $1"
                usage
                exit 1
            fi
            shift
            ;;
    esac
done

if [ -z "$build_dir" ]; then
    usage
    exit 1
fi

mkdir -p "$build_dir"
CONFIG_FILE="$build_dir/.buildconfig"
load_buildconfig

for action in "${ACTIONS[@]}"; do
    case "$action" in
        show)
            show_buildconfig
            exit 0
            ;;
        with:all)
            for name in "${OPT_NAMES[@]}"; do
                config_set "$name" ON
            done
            ;;
        without:all)
            for name in "${OPT_NAMES[@]}"; do
                config_set "$name" OFF
            done
            ;;
        with:*)
            key="${action#with:}"
            if opt_is_known "$key"; then
                config_set "$key" ON
            else
                error "Unknown feature: $key"
                exit 1
            fi
            ;;
        without:*)
            key="${action#without:}"
            if opt_is_known "$key"; then
                config_set "$key" OFF
            else
                error "Unknown feature: $key"
                exit 1
            fi
            ;;
    esac
done

save_buildconfig
show_buildconfig
