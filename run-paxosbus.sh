#!/usr/bin/env bash
set -euo pipefail

# ── Message rate and topology ───────────────────────────────────────────────
MSG_INTERVAL_MS=1      # change this: 1000=1s  100=100ms  10=10ms  2=2ms  1=1ms
NUM_REPLICAS=3
NUM_CLIENTS=2

# ── Gap-agreement knobs (only used with -g) ─────────────────────────────────
GAP_MODE=0             # set by -g; without it, replicas do normal processing
DROP_MOD=2             # non-leaders drop (seq+idx) % DROP_MOD == 0 (recover-from-leader;
                       #   index offset => followers drop DIFFERENT seqs)
NOOP_MOD=0             # ALL replicas drop seq % NOOP_MOD == 0 (NoOp path); 0 = off
DELTA_MS=10            # gap-detection confidence interval Delta (ms)
GAP_RETRY_MS=100       # min spacing between a replica's GapRequest retries per slot;
                       #   must be >= replica->leader RTT or retries storm the leader
RTT_EST_MS="${RTT_EST_MS:-0}"  # expected client->quorum RTT (ms); 0 is fine locally,
                       #   set via env for WAN-like setups so auto resend isn't too small
RESEND_MS=             # client resend-on-no-quorum timeout (ms); blank = auto
                       #   (2*interval + Delta + RTT_EST + 50). Must exceed
                       #   interval+Delta+RTT or the client gives up before a gap can
                       #   even be detected, which snowballs into mass resends
# ────────────────────────────────────────────────────────────────────────────

FORCE_BUILD=0

usage() {
    echo "Usage: $0 [-b] [-p <interval_ms>] [-g] [-d <drop_mod>] [-N <noop_mod>] [-D <delta_ms>] [-R <retry_ms>] [-t <resend_ms>]"
    echo "  -b            force rebuild of Docker image"
    echo "  -p <ms>       message interval in ms (default: $MSG_INTERVAL_MS)"
    echo "  -g            enable gap-agreement mode (default: normal processing)"
    echo "  -d <mod>      non-leaders drop (seq+idx) %% mod == 0 (default: $DROP_MOD)"
    echo "  -N <mod>      ALL replicas drop seq %% mod == 0      (default: $NOOP_MOD, 0=off)"
    echo "  -D <ms>       gap-detection delta in ms             (default: $DELTA_MS)"
    echo "  -R <ms>       GapRequest retry spacing per slot     (default: $GAP_RETRY_MS)"
    echo "  -t <ms>       client resend-on-no-quorum timeout    (default: auto = 2*interval+delta+rtt_est+50)"
    exit 1
}

while getopts "bp:gd:N:D:R:t:h" opt; do
    case $opt in
        b) FORCE_BUILD=1 ;;
        p) MSG_INTERVAL_MS=$OPTARG ;;
        g) GAP_MODE=1 ;;
        d) DROP_MOD=$OPTARG ;;
        N) NOOP_MOD=$OPTARG ;;
        D) DELTA_MS=$OPTARG ;;
        R) GAP_RETRY_MS=$OPTARG ;;
        t) RESEND_MS=$OPTARG ;;
        h) usage ;;
        *) usage ;;
    esac
done

# Auto-size the resend timeout unless overridden with -t. The RTT term matters
# on WAN-like setups: the timeout must exceed the time a quorum could possibly
# take, or every request resends before any reply can physically arrive.
if [[ -z "$RESEND_MS" ]]; then
    RESEND_MS=$(( 2 * MSG_INTERVAL_MS + DELTA_MS + RTT_EST_MS + 50 ))
fi

# Build the per-process gap flags (empty in normal mode).
REPLICA_GAP_FLAGS=()
CLIENT_GAP_FLAGS=()
if [[ $GAP_MODE -eq 1 ]]; then
    REPLICA_GAP_FLAGS=(-g -d "$DROP_MOD" -N "$NOOP_MOD" -D "$DELTA_MS" -R "$GAP_RETRY_MS")
    CLIENT_GAP_FLAGS=(-t "$RESEND_MS")
fi

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
if [[ $GAP_MODE -eq 1 ]]; then
    echo "Mode: GAP AGREEMENT  (dropMod=$DROP_MOD  noopMod=$NOOP_MOD  delta=${DELTA_MS}ms  gapRetry=${GAP_RETRY_MS}ms  resend=${RESEND_MS}ms)"
else
    echo "Mode: NORMAL (no gap agreement)"
fi
echo ""

# ── Per-run log directory (durable copy of every node's stream) ──────────────
RUN_LOG_DIR="$SCRIPT_DIR/logs/local-run-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN_LOG_DIR"
{
    echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_commit=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "interval_ms=$MSG_INTERVAL_MS"
    echo "num_replicas=$NUM_REPLICAS"
    echo "num_clients=$NUM_CLIENTS"
    echo "gap_mode=$GAP_MODE"
    echo "drop_mod=$DROP_MOD"
    echo "noop_mod=$NOOP_MOD"
    echo "delta_ms=$DELTA_MS"
    echo "gap_retry_ms=$GAP_RETRY_MS"
    echo "resend_ms=$RESEND_MS"
} > "$RUN_LOG_DIR/run-meta.txt"
echo "Logs: $RUN_LOG_DIR"
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
            ${REPLICA_GAP_FLAGS[@]+"${REPLICA_GAP_FLAGS[@]}"} \
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
            ${CLIENT_GAP_FLAGS[@]+"${CLIENT_GAP_FLAGS[@]}"} \
        > /dev/null
    CONTAINERS+=("$NAME")
done

echo ""
echo "All containers running."
echo "Clients will sync (5s wait), then stream every ${MSG_INTERVAL_MS}ms."
echo "Press Ctrl+C to stop."
echo "──────────────────────────────────────────────────────────────"

# ── Follow replica logs (tee a durable copy per node into $RUN_LOG_DIR) ──────
for i in $(seq 0 $((NUM_REPLICAS - 1))); do
    docker logs -f --timestamps "paxosbus-replica-$i" 2>&1 \
        | tee "$RUN_LOG_DIR/replica-$i.log" \
        | sed "s/^/[replica-$i] /" &
    LOG_PIDS+=($!)
done

# Also follow client logs so sync/send messages are visible
for i in $(seq 1 $NUM_CLIENTS); do
    docker logs -f --timestamps "paxosbus-client-$i" 2>&1 \
        | tee "$RUN_LOG_DIR/client-$i.log" \
        | sed "s/^/[client-$i]  /" &
    LOG_PIDS+=($!)
done

wait
