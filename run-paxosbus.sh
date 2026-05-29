#!/usr/bin/env bash
set -euo pipefail

# ── Message rate and topology ───────────────────────────────────────────────
MSG_INTERVAL_MS=1000   # change this: 1000=1s  100=100ms  10=10ms  2=2ms
NUM_REPLICAS=3
NUM_CLIENTS=2
# ────────────────────────────────────────────────────────────────────────────

FORCE_BUILD=0

usage() {
    echo "Usage: $0 [-b] [-p <interval_ms>]"
    echo "  -b            force rebuild of Docker image"
    echo "  -p <ms>       message interval in ms (default: $MSG_INTERVAL_MS)"
    exit 1
}

while getopts "bp:h" opt; do
    case $opt in
        b) FORCE_BUILD=1 ;;
        p) MSG_INTERVAL_MS=$OPTARG ;;
        h) usage ;;
        *) usage ;;
    esac
done

SUBNET="172.29.0.0/24"
NETWORK="paxosbus-net"
IMAGE="nopaxos"
BASE_REPLICA_OCTET=10     # replicas: 172.29.0.10 .. .12
BASE_CLIENT_OCTET=100     # clients:  172.29.0.100, .101
REPLICA_PORT=7000

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$SCRIPT_DIR/config"

CONTAINERS=()
LOG_PIDS=()

cleanup() {
    echo ""
    echo "Cleaning up..."
    [[ ${#LOG_PIDS[@]} -gt 0 ]] && kill "${LOG_PIDS[@]}" 2>/dev/null || true
    for c in "${CONTAINERS[@]}"; do
        docker rm -f "$c" &>/dev/null || true
    done
    docker network rm "$NETWORK" &>/dev/null || true
}
trap cleanup EXIT

# ── Build ────────────────────────────────────────────────────────────────────
if [[ $FORCE_BUILD -eq 1 ]] || ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "Building Docker image '$IMAGE'..."
    docker build -t "$IMAGE" "$SCRIPT_DIR"
else
    echo "Using existing Docker image '$IMAGE' (run with -b to rebuild)"
fi

# Verify the PaxosBus binaries exist inside the image
docker run --rm "$IMAGE" test -x /nopaxos/paxosbus/paxosbus-replica || {
    echo "ERROR: /nopaxos/paxosbus/paxosbus-replica not found in image."
    echo "Rebuild the image with: $0 -b"
    exit 1
}

# ── Cleanup stale state ───────────────────────────────────────────────────────
docker ps -aq --filter "name=paxosbus-" | xargs -r docker rm -f &>/dev/null || true
docker network rm "$NETWORK" &>/dev/null || true

# ── Config ───────────────────────────────────────────────────────────────────
mkdir -p "$CONFIG_DIR"
CONF="$CONFIG_DIR/paxosbus.conf"
F=$(( (NUM_REPLICAS - 1) / 2 ))
{
    echo "f $F"
    for i in $(seq 0 $((NUM_REPLICAS - 1))); do
        echo "replica 172.29.0.$((BASE_REPLICA_OCTET + i)):$REPLICA_PORT"
    done
} > "$CONF"

echo "Config ($CONF):"
sed 's/^/  /' "$CONF"
echo ""

# ── Network ──────────────────────────────────────────────────────────────────
docker network create --subnet="$SUBNET" "$NETWORK" > /dev/null

# ── Replicas ─────────────────────────────────────────────────────────────────
for i in $(seq 0 $((NUM_REPLICAS - 1))); do
    NAME="paxosbus-replica-$i"
    IP="172.29.0.$((BASE_REPLICA_OCTET + i))"
    echo "+ replica $NAME  ($IP:$REPLICA_PORT)"
    docker run -d \
        --name "$NAME" \
        --network "$NETWORK" \
        --ip "$IP" \
        -v "$CONFIG_DIR:/config:ro" \
        "$IMAGE" \
        /nopaxos/paxosbus/paxosbus-replica -c /config/paxosbus.conf -i "$i" \
        > /dev/null
    CONTAINERS+=("$NAME")
done

echo "Waiting 2s for replicas to bind..."
sleep 2

# ── Clients ───────────────────────────────────────────────────────────────────
for i in $(seq 1 $NUM_CLIENTS); do
    NAME="paxosbus-client-$i"
    IP="172.29.0.$((BASE_CLIENT_OCTET + i - 1))"
    echo "+ client  $NAME  ($IP  id=$i  interval=${MSG_INTERVAL_MS}ms)"
    docker run -d \
        --name "$NAME" \
        --network "$NETWORK" \
        --ip "$IP" \
        -v "$CONFIG_DIR:/config:ro" \
        "$IMAGE" \
        /nopaxos/paxosbus/paxosbus-client \
            -c /config/paxosbus.conf \
            -I "$i" \
            -p "$MSG_INTERVAL_MS" \
        > /dev/null
    CONTAINERS+=("$NAME")
done

echo ""
echo "All containers running."
echo "Clients will sync (5s wait), then stream every ${MSG_INTERVAL_MS}ms."
echo "Press Ctrl+C to stop."
echo "──────────────────────────────────────────────────────────────"

# ── Follow replica logs ───────────────────────────────────────────────────────
for i in $(seq 0 $((NUM_REPLICAS - 1))); do
    docker logs -f --timestamps "paxosbus-replica-$i" 2>&1 \
        | sed "s/^/[replica-$i] /" &
    LOG_PIDS+=($!)
done

# Also follow client logs so sync/send messages are visible
for i in $(seq 1 $NUM_CLIENTS); do
    docker logs -f --timestamps "paxosbus-client-$i" 2>&1 \
        | sed "s/^/[client-$i]  /" &
    LOG_PIDS+=($!)
done

wait
