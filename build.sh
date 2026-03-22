#!/usr/bin/env bash
# build.sh — developer convenience script for grocy_p4_esp32_terminal
#
# Usage:
#   ./build.sh build               Build firmware for esp32p4
#   ./build.sh config              Open menuconfig
#   ./build.sh flash [PORT]        Flash firmware (default port: auto-detect)
#   ./build.sh monitor [PORT]      Open serial monitor
#   ./build.sh flash-monitor [PORT] Flash then open monitor (requires TTY)
#   ./build.sh flash-log [PORT] [SECS] Flash then capture output (no TTY needed, default 90s)
#   ./build.sh clean               Remove build directory
#   ./build.sh set-target          (Re-)set target to esp32p4
#   ./build.sh test                Build and run unit tests on Linux host
#   ./build.sh size                Print firmware size analysis
#
# PORT defaults to the first /dev/tty.usbserial-* or /dev/ttyUSB* found.

set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/.espressif/v5.4.1/esp-idf}"
# Expand leading ~ in case it was set as a literal string
IDF_PATH="${IDF_PATH/#\~/$HOME}"
IDF_PY="idf.py"  # available on PATH after check_idf sources export.sh

# ── Helpers ──────────────────────────────────────────────────────────────────

die() { echo "error: $*" >&2; exit 1; }

check_idf() {
    [[ -f "$IDF_PATH/tools/idf.py" ]] \
        || die "ESP-IDF not found at $IDF_PATH. Set IDF_PATH to override."
    export IDF_PATH

    # Find the highest-version IDF Python venv that actually exists, so
    # export.sh doesn't fall back to the system Python and pick a missing venv.
    if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
        local venv
        venv="$(ls -d "$HOME/.espressif/python_env/idf5.4_py3"*"_env" 2>/dev/null \
                | sort -V | tail -1)"
        [[ -n "$venv" && -f "$venv/bin/python3" ]] \
            && export IDF_PYTHON_ENV_PATH="$venv"
    fi

    # Source export.sh to activate the venv and add toolchain to PATH.
    # Must NOT use a pipe here — piping runs source in a subshell and
    # PATH/env changes wouldn't propagate back.
    source "$IDF_PATH/export.sh" > /dev/null 2>&1
}

find_port() {
    local port="${1:-}"
    if [[ -n "$port" ]]; then
        echo "$port"
        return
    fi
    # macOS: prefer tty.usbserial-*, fall back to tty.SLAB_*, then cu.*
    for pattern in /dev/tty.usbserial-* /dev/tty.SLAB_* /dev/ttyUSB*; do
        local match
        match="$(compgen -G "$pattern" 2>/dev/null | head -1 || true)"
        if [[ -n "$match" ]]; then
            echo "$match"
            return
        fi
    done
    die "No serial port found. Pass port explicitly: ./build.sh flash /dev/tty.usbserial-XXXX"
}

# ── Commands ─────────────────────────────────────────────────────────────────

cmd_set_target() {
    check_idf
    echo "==> Setting target to esp32p4"
    $IDF_PY set-target esp32p4
}

cmd_build() {
    check_idf
    echo "==> Building firmware"
    $IDF_PY build
}

cmd_config() {
    check_idf
    echo "==> Opening menuconfig"
    $IDF_PY menuconfig
}

cmd_flash() {
    check_idf
    local port; port="$(find_port "${2:-}")"
    echo "==> Flashing to $port"
    $IDF_PY -p "$port" flash
}

cmd_monitor() {
    check_idf
    local port; port="$(find_port "${2:-}")"
    echo "==> Opening monitor on $port (Ctrl-] to exit)"
    $IDF_PY -p "$port" monitor
}

cmd_flash_monitor() {
    check_idf
    local port; port="$(find_port "${2:-}")"
    echo "==> Flashing and monitoring on $port (Ctrl-] to exit)"
    $IDF_PY -p "$port" flash monitor
}

# flash-log: flash + capture serial output without needing a TTY.
# Usage: ./build.sh flash-log [PORT] [TIMEOUT_SECS]
# Useful for automated capture (e.g. from Claude Code's Bash tool).
cmd_flash_log() {
    check_idf
    local port; port="$(find_port "${2:-}")"
    local timeout_s="${3:-90}"
    echo "==> Flashing $port..."
    $IDF_PY -p "$port" flash
    echo "==> Capturing serial output for up to ${timeout_s}s (Ctrl-C to stop)..."
    python3 - "$port" "$timeout_s" <<'PYEOF'
import serial, sys, time, signal

port    = sys.argv[1]
timeout = int(sys.argv[2])

def _exit(sig, frame):
    sys.exit(0)
signal.signal(signal.SIGINT,  _exit)
signal.signal(signal.SIGTERM, _exit)

with serial.Serial(port, 115200, timeout=1) as s:
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = s.readline()
        if line:
            print(line.decode('utf-8', errors='replace'), end='', flush=True)
PYEOF
}

cmd_clean() {
    echo "==> Removing build/"
    rm -rf build/
}

cmd_size() {
    check_idf
    echo "==> Firmware size analysis"
    $IDF_PY size-components
}

cmd_test() {
    check_idf
    echo "==> Building unit tests (Linux host target)"
    pushd test_app > /dev/null
    $IDF_PY build
    echo ""
    echo "==> Running tests"
    IDF_PATH="$IDF_PATH" ./build/grocy_unit_tests.elf &
    TEST_PID=$!
    sleep 5
    kill "$TEST_PID" 2>/dev/null || true
    wait "$TEST_PID" 2>/dev/null || true
    popd > /dev/null
}

# ── Dispatch ─────────────────────────────────────────────────────────────────

COMMAND="${1:-help}"

case "$COMMAND" in
    build)         cmd_build ;;
    config)        cmd_config ;;
    flash)         cmd_flash "$@" ;;
    monitor)       cmd_monitor "$@" ;;
    flash-monitor) cmd_flash_monitor "$@" ;;
    flash-log)     cmd_flash_log "$@" ;;
    clean)         cmd_clean ;;
    set-target)    cmd_set_target ;;
    test)          cmd_test ;;
    size)          cmd_size ;;
    help|--help|-h)
        sed -n '3,13p' "$0"   # print the usage comment at the top
        ;;
    *)
        echo "Unknown command: $COMMAND"
        echo "Run './build.sh help' for usage."
        exit 1
        ;;
esac
