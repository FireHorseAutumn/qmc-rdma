#!/usr/bin/env bash
# 02_perftest — bandwidth and latency sweeps across the SoftRoCE fabric.
# Fabric must be up first: sudo ../01_bringup/bringup.sh up
#
#   sudo ./run_sweeps.sh
#
# Server in ns1 on rxe0 (10.0.0.1), client in ns2 on rxe1 (10.0.0.2).
# Raw tool output lands in raw/ next to this script; nothing is summarized
# here — the numbers table in RESULTS.md is written from these files.
set -euo pipefail
trap 'echo "FAILED at line $LINENO: $BASH_COMMAND" >&2' ERR
cd "$(dirname "$0")"
mkdir -p raw
: > raw/STATUS.txt

SRV="ip netns exec ns1"
CLI="ip netns exec ns2"
SRV_DEV=rxe0
CLI_DEV=rxe1
SERVER_IP=10.0.0.1

run_test() {
    local name="$1"; shift
    local prog="$1"; shift
    # "$@" = extra args common to both sides (e.g. -s SIZE, -t TXDEPTH)
    # Server output, client output, and both exit statuses are all preserved;
    # a failed pair is recorded in STATUS.txt and the sweep continues.
    echo "=== $name ==="
    $SRV "$prog" -d "$SRV_DEV" "$@" >"raw/${name}.server.txt" 2>&1 &
    local srv_pid=$!
    sleep 1
    local cli_rc srv_rc
    set +e
    $CLI "$prog" -d "$CLI_DEV" "$@" "$SERVER_IP" 2>&1 | tee "raw/${name}.client.txt"
    cli_rc=${PIPESTATUS[0]}
    wait "$srv_pid"
    srv_rc=$?
    set -e
    echo "$name client_rc=$cli_rc server_rc=$srv_rc" | tee -a raw/STATUS.txt
    sleep 1
}

# --- headline numbers, default message sizes ---
run_test ib_write_bw   ib_write_bw
run_test ib_read_bw    ib_read_bw
run_test ib_send_lat   ib_send_lat

# --- message-size sweep, bandwidth (write) ---
for sz in 1024 4096 65536 1048576; do
    run_test "ib_write_bw_s${sz}" ib_write_bw -s "$sz"
done

# --- message-size sweep, latency (send) ---
for sz in 16 256 4096 65536; do
    run_test "ib_send_lat_s${sz}" ib_send_lat -s "$sz"
done

# --- TX-depth sweep: in-flight WQEs at fixed 64 KiB ---
# -t is the TX queue depth; -q would change the QP count instead.
for d in 1 32 128; do
    run_test "ib_write_bw_txd${d}" ib_write_bw -s 65536 -t "$d"
done

echo
echo "done. raw output in $(pwd)/raw/"
