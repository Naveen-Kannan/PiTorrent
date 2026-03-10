#!/bin/bash
#
# Two-step BitTorrent-over-Bluetooth test harness.
#
#   ./bt-test.sh send [input_file]   — bootload all connected Pis, distribute file
#   ./bt-test.sh listen [port]       — bootload one Pi, join with 0 chunks
#
# IMPORTANT: power-cycle all Pis before running.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="btnode.bin"
BIN_PATH="$SCRIPT_DIR/$BIN"
STATE_FILE="$SCRIPT_DIR/.demo_state.json"

usage() {
    cat <<EOF
BitTorrent over Bluetooth — test harness

Usage:
  $0 send [input_file]    Bootload all Pis, distribute file (default: input.txt)
  $0 listen [port]        Bootload one Pi as listener (0 chunks)

Send mode:
  1. Auto-discovers all /dev/cu.usbserial-* ports
  2. Bootloads btnode.bin to each Pi sequentially via my-install
  3. Runs demo.py to distribute chunks and monitor output
  4. Saves state to .demo_state.json for listener mode

Listen mode:
  1. Requires .demo_state.json from a prior 'send' run
     (copy it from the seeder Mac if running on a different machine)
  2. Bootloads btnode.bin to one Pi
  3. Pi joins the swarm with 0 chunks, downloads everything over Bluetooth

Power-cycle all Pis before running either mode.
EOF
    exit 1
}

discover_ports() {
    local ports
    ports=$(ls /dev/cu.usbserial-* 2>/dev/null | sort)
    if [ -z "$ports" ]; then
        echo "ERROR: no /dev/cu.usbserial-* devices found. Are Pis plugged in?" >&2
        exit 1
    fi
    echo "$ports"
}

bootload_one() {
    local port="$1"
    local short="${port##*-}"
    local logfile
    logfile=$(mktemp /tmp/bt-boot-XXXXXX)

    echo "  [$short] bootloading..."

    # my-install doesn't exit after boot — it stays as a serial console.
    # Run it in background, watch for the success marker, then kill it.
    my-install "$port" "$BIN_PATH" >"$logfile" 2>&1 &
    local pid=$!

    local elapsed=0
    local timeout=30
    while [ $elapsed -lt $timeout ]; do
        # check for success
        if grep -q "bootloader: Done" "$logfile" 2>/dev/null; then
            echo "  [$short] OK"
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            rm -f "$logfile"
            return 0
        fi
        # check if my-install exited on its own (error)
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "  [$short] FAILED — my-install exited unexpectedly:"
            cat "$logfile"
            rm -f "$logfile"
            exit 1
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    # timed out
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "  [$short] TIMED OUT after ${timeout}s. Power-cycle the Pi and retry."
    echo "  my-install output:"
    cat "$logfile"
    rm -f "$logfile"
    exit 1
}

# --- sanity checks ---

if [ ! -f "$BIN_PATH" ]; then
    echo "ERROR: $BIN_PATH not found. Run 'make' first."
    exit 1
fi

command -v my-install >/dev/null 2>&1 || {
    echo "ERROR: my-install not found on PATH."
    exit 1
}

command -v python3 >/dev/null 2>&1 || {
    echo "ERROR: python3 not found on PATH."
    exit 1
}

# --- main ---

case "${1:-}" in
send)
    INPUT_FILE="${2:-input.txt}"

    # resolve relative to script dir if not an absolute path
    if [[ "$INPUT_FILE" != /* ]]; then
        if [ -f "$SCRIPT_DIR/$INPUT_FILE" ]; then
            INPUT_FILE="$SCRIPT_DIR/$INPUT_FILE"
        elif [ ! -f "$INPUT_FILE" ]; then
            echo "ERROR: $INPUT_FILE not found"
            exit 1
        fi
    fi

    PORTS=()
    for p in $(discover_ports); do PORTS+=("$p"); done
    N=${#PORTS[@]}

    echo "=== BitTorrent over Bluetooth: SEND ==="
    echo "Input : $INPUT_FILE"
    echo "Binary: $BIN"
    echo "Pis   : $N  (${PORTS[*]})"
    echo ""

    echo "--- Bootloading $N Pi(s) ---"
    for port in "${PORTS[@]}"; do
        bootload_one "$port"
    done
    echo "All Pis bootloaded."
    echo ""

    echo "Waiting 2s for Bluetooth firmware upload..."
    sleep 2

    echo "--- Starting chunk distribution ---"
    python3 "$SCRIPT_DIR/demo.py" send "$INPUT_FILE" "${PORTS[@]}"
    ;;

listen)
    PORT="${2:-}"

    if [ -z "$PORT" ]; then
        PORTS=()
        for p in $(discover_ports); do PORTS+=("$p"); done
        PORT="${PORTS[0]}"
        echo "(auto-detected port: $PORT)"
    fi

    if [ ! -f "$STATE_FILE" ]; then
        echo "ERROR: $STATE_FILE not found."
        echo ""
        echo "Run '$0 send' on the seeder Mac first."
        echo "If the seeder is on a different Mac, copy .demo_state.json here:"
        echo "  scp seeder-mac:path/to/bittorrent/.demo_state.json $SCRIPT_DIR/"
        exit 1
    fi

    echo "=== BitTorrent over Bluetooth: LISTEN ==="
    echo "Port  : $PORT"
    echo "State : $STATE_FILE"
    echo ""

    echo "--- Bootloading listener Pi ---"
    bootload_one "$PORT"
    echo ""

    echo "Waiting 2s for Bluetooth firmware upload..."
    sleep 2

    echo "--- Starting listener ---"
    python3 "$SCRIPT_DIR/demo.py" listen "$PORT"
    ;;

*)
    usage
    ;;
esac
