#!/usr/bin/env bash
set -euo pipefail

REPLICAS=3
PROTOCOL=nopaxos
REQUESTS=100
FORCE_BUILD=0

usage() {
    echo "Usage: $0 [-n NUM_REPLICAS] [-m PROTOCOL] [-r NUM_REQUESTS] [-b]"
    echo "  -n  number of replicas (default: 3, min: 3)"
    echo "  -m  protocol: nopaxos | vr | spec | fastpaxos | unreplicated (default: nopaxos)"
    echo "  -r  number of client requests (default: 100)"
    echo "  -b  force rebuild of Docker image"
    exit 1
}

while getopts "n:m:r:bh" opt; do
    case $opt in
        n) REPLICAS=$OPTARG ;;
        m) PROTOCOL=$OPTARG ;;
        r) REQUESTS=$OPTARG ;;
        b) FORCE_BUILD=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [[ $REPLICAS -lt 3 ]]; then
    echo "error: -n must be at least 3 (minimum 2f+1 for f=1)" >&2
    exit 1
fi

case $PROTOCOL in
    nopaxos|vr|spec|fastpaxos|unreplicated) ;;
    *) echo "error: unknown protocol '$PROTOCOL'" >&2; usage ;;
esac

SUBNET="172.28.0.0/24"
NETWORK="nopaxos-net"
IMAGE="nopaxos"
SEQ_IP="172.28.0.2"
BASE_REPLICA_IP_THIRD_OCTET=10
CLIENT_IP="172.28.0.100"
MULTICAST_IP="172.28.0.255"
REPLICA_PORT=7000
MULTICAST_PORT=9000

F=$(( (REPLICAS - 1) / 2 ))

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$SCRIPT_DIR/config"

#cleanup
CONTAINERS=()

cleanup() {
    echo ""
    echo "Cleaning up..."
    for c in "${CONTAINERS[@]}"; do
        docker rm -f "$c" &>/dev/null || true
    done
    docker network rm "$NETWORK" &>/dev/null || true
}
trap cleanup EXIT

#config files
mkdir -p "$CONFIG_DIR"

#replica and client config
REPLICA_CONF="$CONFIG_DIR/replica.conf"
{
    echo "f $F"
    for i in $(seq 0 $((REPLICAS - 1))); do
        echo "replica 172.28.0.$((BASE_REPLICA_IP_THIRD_OCTET + i)):$REPLICA_PORT"
    done
    if [[ $PROTOCOL == nopaxos ]]; then
        echo "multicast $MULTICAST_IP:$MULTICAST_PORT"
    fi
} > "$REPLICA_CONF"

SEQ_CONF="$CONFIG_DIR/sequencer.conf"
if [[ $PROTOCOL == nopaxos ]]; then
    {
        echo "interface eth0"
        echo "groupaddr $MULTICAST_IP"
    } > "$SEQ_CONF"
fi

#build docker image
if [[ $FORCE_BUILD -eq 1 ]] || ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "Building Docker image '$IMAGE'..."
    docker build -t "$IMAGE" "$SCRIPT_DIR"
else
    echo "Using existing Docker image '$IMAGE' (run with -b to rebuild)"
fi

#cleanup
docker ps -aq --filter "name=nopaxos-" | xargs -r docker rm -f &>/dev/null || true
docker network rm "$NETWORK" &>/dev/null || true

docker network create --subnet="$SUBNET" "$NETWORK" > /dev/null

#start sequencer
if [[ $PROTOCOL == nopaxos ]]; then
    docker run -d \
        --name nopaxos-sequencer \
        --network "$NETWORK" \
        --ip "$SEQ_IP" \
        --cap-add NET_RAW \
        -v "$CONFIG_DIR:/config:ro" \
        "$IMAGE" \
        /nopaxos/sequencer/sequencer -c /config/sequencer.conf \
        > /dev/null
    CONTAINERS+=(nopaxos-sequencer)
fi

#start replicas
for i in $(seq 0 $((REPLICAS - 1))); do
    NAME="nopaxos-replica-$i"
    IP="172.28.0.$((BASE_REPLICA_IP_THIRD_OCTET + i))"
    echo "+ starting replica $NAME"
    docker run -d \
        --name "$NAME" \
        --network "$NETWORK" \
        --ip "$IP" \
        -v "$CONFIG_DIR:/config:ro" \
        "$IMAGE" \
        /nopaxos/bench/replica -c /config/replica.conf -i "$i" -m "$PROTOCOL" \
        > /dev/null
    CONTAINERS+=("$NAME")
done

sleep 3

#run client
echo "+ running client ($REQUESTS requests)"
docker run --rm \
    --name nopaxos-client \
    --network "$NETWORK" \
    --ip "$CLIENT_IP" \
    -v "$CONFIG_DIR:/config:ro" \
    "$IMAGE" \
    /nopaxos/bench/client -c /config/replica.conf -m "$PROTOCOL" -n "$REQUESTS"
