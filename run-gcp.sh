#!/usr/bin/env bash
set -euo pipefail

# Required: REPO_URL  (e.g. https://github.com/austint903/PaxosBus.git)
# Optional: INTERVAL_MS (default 100), DURATION_S (default 60)
#
# Architecture:
#   pb-controller  (us-east1-c, external IP)  → jump host / orchestrator
#     ├─ pb-useast1        (us-east1-d)          replica 0
#     ├─ pb-europenorth1   (europe-north1-c)     replica 1
#     ├─ pb-southamerica   (southamerica-east1-b) replica 2
#     └─ pb-asia1          (asia-east1-c)        2 clients
#
# Only pb-controller has an external IP. All other VMs are reached via
# `gcloud compute ssh --internal-ip` from the controller (gcloud is
# pre-installed on GCE VMs and authenticates via the VM's service account;
# the SA needs compute.osLogin or compute scope).

: "${REPO_URL:?Set REPO_URL=https://github.com/austint903/PaxosBus.git}"
INTERVAL_MS="${INTERVAL_MS:-100}"
DURATION_S="${DURATION_S:-60}"

CONTROLLER_VM="pb-controller"
CONTROLLER_ZONE="us-east1-c"

echo "==> gcloud compute instances list (discovering pb-* VMs)"
mapfile -t INSTANCES < <(gcloud compute instances list \
  --filter="name~^pb-" \
  --format="value(name,zone,networkInterfaces[0].networkIP)")

REPLICA0_VM= REPLICA0_ZONE= REPLICA0_IP=  # us-east  (not controller)
REPLICA1_VM= REPLICA1_ZONE= REPLICA1_IP=  # europe
REPLICA2_VM= REPLICA2_ZONE= REPLICA2_IP=  # south america
CLIENT_VM=   CLIENT_ZONE=   CLIENT_IP=    # asia (2 clients)

for line in "${INSTANCES[@]}"; do
  read -r name zone ip <<< "$line"
  [[ "$name" == "$CONTROLLER_VM" ]] && continue
  case "$zone" in
    us-east*)      REPLICA0_VM=$name; REPLICA0_ZONE=$zone; REPLICA0_IP=$ip ;;
    europe*)       REPLICA1_VM=$name; REPLICA1_ZONE=$zone; REPLICA1_IP=$ip ;;
    southamerica*) REPLICA2_VM=$name; REPLICA2_ZONE=$zone; REPLICA2_IP=$ip ;;
    asia*)         CLIENT_VM=$name;   CLIENT_ZONE=$zone;   CLIENT_IP=$ip ;;
  esac
done

for slot in REPLICA0_VM REPLICA1_VM REPLICA2_VM CLIENT_VM; do
  [[ -n "${!slot}" ]] || { echo "MISSING $slot — check VM zones / names"; exit 1; }
done

printf "  CTRL  %-20s %-22s (entry point)\n" "$CONTROLLER_VM" "$CONTROLLER_ZONE"
printf "  R0    %-20s %-22s %s\n" "$REPLICA0_VM" "$REPLICA0_ZONE" "$REPLICA0_IP"
printf "  R1    %-20s %-22s %s\n" "$REPLICA1_VM" "$REPLICA1_ZONE" "$REPLICA1_IP"
printf "  R2    %-20s %-22s %s\n" "$REPLICA2_VM" "$REPLICA2_ZONE" "$REPLICA2_IP"
printf "  CL    %-20s %-22s %s\n" "$CLIENT_VM"   "$CLIENT_ZONE"   "$CLIENT_IP"

echo "==> Ensuring all VMs are RUNNING"
for vm_zone in "$CONTROLLER_VM:$CONTROLLER_ZONE" \
               "$REPLICA0_VM:$REPLICA0_ZONE" \
               "$REPLICA1_VM:$REPLICA1_ZONE" \
               "$REPLICA2_VM:$REPLICA2_ZONE" \
               "$CLIENT_VM:$CLIENT_ZONE"; do
  vm="${vm_zone%%:*}"; zone="${vm_zone##*:}"
  status=$(gcloud compute instances describe "$vm" --zone="$zone" --format="value(status)")
  if [[ "$status" != "RUNNING" ]]; then
    echo "  starting $vm (was $status)"
    gcloud compute instances start "$vm" --zone="$zone" --quiet &
  fi
done
wait
sleep 10  # give sshd time to come up on freshly-started VMs

# ---------------------------------------------------------------------------
# Generate the orchestrator script that will run *on* pb-controller.
# Quoted heredoc → no local variable expansion; all inputs come via env vars
# passed on the gcloud ssh command line.
# ---------------------------------------------------------------------------
ORCH=$(mktemp)
cat > "$ORCH" <<'ORCH_EOF'
#!/usr/bin/env bash
set -euo pipefail

: "${REPO_URL:?}" "${INTERVAL_MS:?}" "${DURATION_S:?}"
: "${REPLICA0_VM:?}" "${REPLICA0_ZONE:?}" "${REPLICA0_IP:?}"
: "${REPLICA1_VM:?}" "${REPLICA1_ZONE:?}" "${REPLICA1_IP:?}"
: "${REPLICA2_VM:?}" "${REPLICA2_ZONE:?}" "${REPLICA2_IP:?}"
: "${CLIENT_VM:?}"   "${CLIENT_ZONE:?}"   "${CLIENT_IP:?}"

ssh_to()   { gcloud compute ssh "$1" --zone="$2" --internal-ip --quiet -- "$3"; }
scp_to()   { gcloud compute scp --zone="$2" --internal-ip --quiet "$3" "$1":"$4"; }
scp_from() { gcloud compute scp --zone="$2" --internal-ip --quiet "$1":"$3" "$4"; }

echo "[ctrl] Build on $REPLICA0_VM (git pull + incremental make)"
ssh_to "$REPLICA0_VM" "$REPLICA0_ZONE" "
  set -e
  sudo apt-get update -qq
  sudo apt-get install -y -qq build-essential g++ protobuf-compiler pkg-config \
       libunwind-dev libssl-dev libprotobuf-dev libevent-dev libgtest-dev git
  if [[ ! -d ~/PaxosBus ]]; then
    git clone '$REPO_URL' ~/PaxosBus
  else
    git -C ~/PaxosBus pull --ff-only
  fi
  cd ~/PaxosBus && make -j8
  mkdir -p ~/paxosbus
  cp paxosbus/paxosbus-replica paxosbus/paxosbus-client ~/paxosbus/
  chmod +x ~/paxosbus/*
"

echo "[ctrl] Install runtime libs on R1, R2, CL (parallel)"
for vm_zone in "$REPLICA1_VM:$REPLICA1_ZONE" "$REPLICA2_VM:$REPLICA2_ZONE" "$CLIENT_VM:$CLIENT_ZONE"; do
  vm="${vm_zone%%:*}"; zone="${vm_zone##*:}"
  ssh_to "$vm" "$zone" \
    "sudo apt-get update -qq && sudo apt-get install -y -qq libprotobuf-dev libevent-dev libunwind-dev libssl-dev" &
done
wait

echo "[ctrl] Pull binaries from builder to controller, then fan out"
TMPBIN=$(mktemp -d)
trap 'rm -rf "$TMPBIN"' EXIT
scp_from "$REPLICA0_VM" "$REPLICA0_ZONE" "paxosbus/paxosbus-replica" "$TMPBIN/"
scp_from "$REPLICA0_VM" "$REPLICA0_ZONE" "paxosbus/paxosbus-client"  "$TMPBIN/"

for vm_zone in "$REPLICA1_VM:$REPLICA1_ZONE" "$REPLICA2_VM:$REPLICA2_ZONE" "$CLIENT_VM:$CLIENT_ZONE"; do
  vm="${vm_zone%%:*}"; zone="${vm_zone##*:}"
  ssh_to "$vm" "$zone" "mkdir -p ~/paxosbus"
  scp_to "$vm" "$zone" "$TMPBIN/paxosbus-replica" "paxosbus/"
  scp_to "$vm" "$zone" "$TMPBIN/paxosbus-client"  "paxosbus/"
  ssh_to "$vm" "$zone" "chmod +x ~/paxosbus/*"
done

echo "[ctrl] Generate + distribute paxosbus.conf"
CONFFILE=$(mktemp)
cat > "$CONFFILE" <<CONF
f 1
replica $REPLICA0_IP:7000
replica $REPLICA1_IP:7000
replica $REPLICA2_IP:7000
CONF

for vm_zone in "$REPLICA0_VM:$REPLICA0_ZONE" "$REPLICA1_VM:$REPLICA1_ZONE" "$REPLICA2_VM:$REPLICA2_ZONE" "$CLIENT_VM:$CLIENT_ZONE"; do
  vm="${vm_zone%%:*}"; zone="${vm_zone##*:}"
  scp_to "$vm" "$zone" "$CONFFILE" "paxosbus/paxosbus.conf"
done
rm "$CONFFILE"

echo "[ctrl] Kill any stale processes"
for slot in 0 1 2; do
  vm_var="REPLICA${slot}_VM"; zone_var="REPLICA${slot}_ZONE"
  ssh_to "${!vm_var}" "${!zone_var}" "pkill -f paxosbus-replica || true" &
done
ssh_to "$CLIENT_VM" "$CLIENT_ZONE" "pkill -f paxosbus-client || true" &
wait
sleep 2

echo "[ctrl] Launch replicas (us-east, europe, south-america)"
for slot in 0 1 2; do
  vm_var="REPLICA${slot}_VM"; zone_var="REPLICA${slot}_ZONE"
  ssh_to "${!vm_var}" "${!zone_var}" \
    "rm -f /tmp/paxosbus.log; nohup ~/paxosbus/paxosbus-replica \
       -c ~/paxosbus/paxosbus.conf -i $slot > /tmp/paxosbus.log 2>&1 &"
done
sleep 3

echo "[ctrl] Launch 2 clients on $CLIENT_VM (asia) — pinging replicas"
for id in 1 2; do
  ssh_to "$CLIENT_VM" "$CLIENT_ZONE" \
    "rm -f /tmp/paxosbus-client-$id.log; nohup ~/paxosbus/paxosbus-client \
       -c ~/paxosbus/paxosbus.conf -I $id -p $INTERVAL_MS \
       > /tmp/paxosbus-client-$id.log 2>&1 &"
done

echo ""
echo "[ctrl] Live tail of client 1 (running for $((DURATION_S + 6))s)"
echo "----------------------------------------------------------------"
gcloud compute ssh "$CLIENT_VM" --zone="$CLIENT_ZONE" --internal-ip --quiet \
  -- "tail -f /tmp/paxosbus-client-1.log" &
TAIL_PID=$!
sleep $((DURATION_S + 6))
kill "$TAIL_PID" 2>/dev/null || true
wait "$TAIL_PID" 2>/dev/null || true

echo "----------------------------------------------------------------"
echo "[ctrl] Stopping replicas + clients"
for slot in 0 1 2; do
  vm_var="REPLICA${slot}_VM"; zone_var="REPLICA${slot}_ZONE"
  ssh_to "${!vm_var}" "${!zone_var}" "pkill -f paxosbus-replica || true" &
done
ssh_to "$CLIENT_VM" "$CLIENT_ZONE" "pkill -f paxosbus-client || true" &
wait

echo "[ctrl] Collecting logs into ~/paxosbus-logs/ on controller"
rm -rf ~/paxosbus-logs && mkdir -p ~/paxosbus-logs
for slot in 0 1 2; do
  vm_var="REPLICA${slot}_VM"; zone_var="REPLICA${slot}_ZONE"
  scp_from "${!vm_var}" "${!zone_var}" "/tmp/paxosbus.log" "$HOME/paxosbus-logs/${!vm_var}.log"
done
scp_from "$CLIENT_VM" "$CLIENT_ZONE" "/tmp/paxosbus-client-1.log" "$HOME/paxosbus-logs/"
scp_from "$CLIENT_VM" "$CLIENT_ZONE" "/tmp/paxosbus-client-2.log" "$HOME/paxosbus-logs/"

echo ""
echo "[ctrl] Per-replica RTT summary"
for c in 1 2; do
  echo "=== client $c ==="
  for r in 0 1 2; do
    grep -oE "REPLY from replica=$r  rtt=[0-9]+us" "$HOME/paxosbus-logs/paxosbus-client-$c.log" 2>/dev/null \
      | grep -oE "[0-9]+" \
      | awk -v r=$r '{a[NR]=$1; s+=$1} END {
          n=NR; if (!n) { print "  replica="r" no data"; exit }
          asort(a);
          printf "  replica=%d  n=%d  avg=%.0fus  p50=%dus  p99=%dus\n",
                 r, n, s/n, a[int(n*0.5)], a[int(n*0.99)] }'
  done
done

echo "[ctrl] Done. Logs in ~/paxosbus-logs/ on $(hostname)"
ORCH_EOF

echo "==> Uploading orchestrator to $CONTROLLER_VM"
gcloud compute scp --zone="$CONTROLLER_ZONE" --quiet "$ORCH" "$CONTROLLER_VM":~/orchestrator.sh
rm "$ORCH"

echo "==> Running orchestrator on $CONTROLLER_VM (fans out to R0/R1/R2/CL)"
gcloud compute ssh "$CONTROLLER_VM" --zone="$CONTROLLER_ZONE" --quiet -- "
  REPO_URL='$REPO_URL' \
  INTERVAL_MS='$INTERVAL_MS' \
  DURATION_S='$DURATION_S' \
  REPLICA0_VM='$REPLICA0_VM' REPLICA0_ZONE='$REPLICA0_ZONE' REPLICA0_IP='$REPLICA0_IP' \
  REPLICA1_VM='$REPLICA1_VM' REPLICA1_ZONE='$REPLICA1_ZONE' REPLICA1_IP='$REPLICA1_IP' \
  REPLICA2_VM='$REPLICA2_VM' REPLICA2_ZONE='$REPLICA2_ZONE' REPLICA2_IP='$REPLICA2_IP' \
  CLIENT_VM='$CLIENT_VM'     CLIENT_ZONE='$CLIENT_ZONE'     CLIENT_IP='$CLIENT_IP' \
  bash ~/orchestrator.sh
"

echo "==> Copying logs from $CONTROLLER_VM to ./logs/"
mkdir -p ./logs
gcloud compute scp --zone="$CONTROLLER_ZONE" --quiet --recurse \
  "$CONTROLLER_VM":~/paxosbus-logs/. ./logs/

echo "==> Done. VMs left running. Logs in ./logs/"
