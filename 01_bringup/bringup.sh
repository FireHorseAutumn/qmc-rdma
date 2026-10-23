#!/usr/bin/env bash
# 01_bringup — SoftRoCE (RXE) two-node fabric on one host.
# Two network namespaces ("nodes") connected by a veth pair;
# one RXE device attached to each namespace's veth endpoint.
#
#   sudo ./bringup.sh up     # create fabric, verify
#   sudo ./bringup.sh down   # tear everything down
#
#   ns1: 10.0.0.1/24 on veth-a, RXE device rxe0
#   ns2: 10.0.0.2/24 on veth-b, RXE device rxe1
#
# NOTE: rdma netns mode is "shared", so RDMA devices are visible in every
# namespace — the two RXE devices MUST have distinct names (rxe0/rxe1).
# Same name collided globally and killed the first run with a misleading
# ENFILE ("Too many open files in system"). See ../LOG.md.
set -euo pipefail
trap 'echo "FAILED at line $LINENO: $BASH_COMMAND" >&2' ERR

cmd="${1:-up}"

down() {
    rdma link del rxe0 2>/dev/null || true   # shared mode: visible from host
    rdma link del rxe1 2>/dev/null || true
    ip netns del ns1 2>/dev/null || true
    ip netns del ns2 2>/dev/null || true
    echo "[down] namespaces and RXE links removed (rdma_rxe module left loaded)"
}

up() {
    modprobe rdma_rxe

    # RXE link creation inside netns requires shared rdma netns mode (default).
    mode="$(rdma system show 2>/dev/null || true)"
    echo "[up] rdma system: ${mode:-unknown}"
    if ! grep -q "netns shared" <<<"$mode"; then
        echo "[up] setting rdma netns mode to shared"
        rdma system set netns shared
    fi

    ip netns add ns1
    ip netns add ns2

    ip link add veth-a type veth peer name veth-b
    ip link set veth-a netns ns1
    ip link set veth-b netns ns2

    ip netns exec ns1 ip addr add 10.0.0.1/24 dev veth-a
    ip netns exec ns2 ip addr add 10.0.0.2/24 dev veth-b
    ip netns exec ns1 ip link set veth-a up
    ip netns exec ns2 ip link set veth-b up
    ip netns exec ns1 ip link set lo up
    ip netns exec ns2 ip link set lo up

    # One RXE device per namespace, on that namespace's veth endpoint.
    # Distinct names: in shared netns mode the devices are global.
    ip netns exec ns1 rdma link add rxe0 type rxe netdev veth-a
    ip netns exec ns2 rdma link add rxe1 type rxe netdev veth-b

    echo
    echo "=== connectivity: ns1 -> ns2 ==="
    ip netns exec ns1 ping -c 2 -W 1 10.0.0.2

    echo
    echo "=== rdma links (host view; shared mode) ==="
    rdma link show

    echo
    echo "=== ibv_devices (ns1 / ns2) ==="
    ip netns exec ns1 ibv_devices
    ip netns exec ns2 ibv_devices

    echo
    echo "=== ibstat rxe0 (ns1) / ibstat rxe1 (ns2) ==="
    ip netns exec ns1 ibstat rxe0
    ip netns exec ns2 ibstat rxe1

    echo
    echo "[up] fabric is live: ns1(10.0.0.1, rxe0) <-> ns2(10.0.0.2, rxe1)"
}

case "$cmd" in
    up)   down >/dev/null 2>&1 || true; up ;;
    down) down ;;
    *)    echo "usage: $0 {up|down}" >&2; exit 1 ;;
esac
