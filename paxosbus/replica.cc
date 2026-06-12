// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#include "paxosbus/replica.h"
#include "lib/message.h"
#include "lib/assert.h"

#include <ctime>
#include <cinttypes>
#include <algorithm>

namespace specpaxos {
namespace paxosbus {

PaxosBusReplica::PaxosBusReplica(const specpaxos::Configuration &config,
                                  int replicaIdx,
                                  Transport *transport,
                                  bool gapEnabled,
                                  uint64_t dropMod,
                                  uint64_t noopMod,
                                  uint64_t deltaMs,
                                  uint64_t gapRetryMs)
    : config(config), replicaIdx(replicaIdx), view_id_(0), transport(transport),
      gapEnabled(gapEnabled), dropMod(dropMod), noopMod(noopMod),
      deltaMs(deltaMs), gapRetryMs(gapRetryMs),
      winRecv(0), winDrops(0), winGaps(0), winRecovered(0), winNoops(0),
      winDeltaSumUs(0), winDeltaMinUs(0), winDeltaMaxUs(0)
{
    transport->Register(this, config, 0, replicaIdx);
    Notice("[Replica %d] started (view=0, f=%d, quorum=%d, leader=%s)",
           replicaIdx, config.f, config.QuorumSize(),
           AmLeader() ? "yes" : "no");
    if (gapEnabled) {
        Notice("[Replica %d] gap mode ON: dropMod=%" PRIu64 " noopMod=%" PRIu64
               " delta=%" PRIu64 "ms gapRetry=%" PRIu64 "ms",
               replicaIdx, dropMod, noopMod, deltaMs, gapRetryMs);
    }
    transport->Timer(1000, [this]() { OnStatsTimer(); });
}

PaxosBusReplica::~PaxosBusReplica()
{
    for (auto &kv : clients) {
        if (kv.second.gapTimerId != 0) {
            transport->CancelTimer(kv.second.gapTimerId);
        }
    }
}

uint64_t
PaxosBusReplica::NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

bool
PaxosBusReplica::ShouldDrop(uint64_t seq) const
{
    // NoOp knob: every replica (incl. leader) drops -> no one has it -> gap agreement.
    if (noopMod > 0 && seq % noopMod == 0) {
        return true;
    }
    // Drop knob: only non-leaders drop -> leader still has it -> recover-from-leader.
    // Offset by replica index so different followers drop DIFFERENT seqs and a
    // leader-inclusive quorum can still form from the other follower.
    if (dropMod > 0 && !AmLeader() &&
        (seq + (uint64_t)replicaIdx) % dropMod == 0) {
        return true;
    }
    return false;
}

void
PaxosBusReplica::ReceiveMessage(const TransportAddress &remote,
                                 const string &type,
                                 const string &data,
                                 void *meta_data)
{
    static ::paxosbus::proto::SyncMessage syncMsg;
    static ::paxosbus::proto::DataMessage dataMsg;
    static ::paxosbus::proto::GapRequestMessage gapReq;
    static ::paxosbus::proto::GapReplyMessage gapReply;
    static ::paxosbus::proto::GapCommitMessage gapCommit;
    static ::paxosbus::proto::GapCommitReplyMessage gapCommitReply;

    if (type == syncMsg.GetTypeName()) {
        syncMsg.ParseFromString(data);
        HandleSync(remote, syncMsg);
    } else if (type == dataMsg.GetTypeName()) {
        dataMsg.ParseFromString(data);
        HandleData(remote, dataMsg);
    } else if (type == gapReq.GetTypeName()) {
        gapReq.ParseFromString(data);
        HandleGapRequest(remote, gapReq);
    } else if (type == gapReply.GetTypeName()) {
        gapReply.ParseFromString(data);
        HandleGapReply(remote, gapReply);
    } else if (type == gapCommit.GetTypeName()) {
        gapCommit.ParseFromString(data);
        HandleGapCommit(remote, gapCommit);
    } else if (type == gapCommitReply.GetTypeName()) {
        gapCommitReply.ParseFromString(data);
        HandleGapCommitReply(remote, gapCommitReply);
    } else {
        Warning("Replica %d: unknown message type %s", replicaIdx, type.c_str());
    }
}

void
PaxosBusReplica::HandleSync(const TransportAddress &remote,
                            const ::paxosbus::proto::SyncMessage &msg)
{
    int64_t recv_ns = (int64_t)NowNs();
    ClientStream &s = clients[msg.client_id()];
    // Anchor baseline to when the first data message is expected to arrive:
    // base = sync_recv + start_delay, so expected(N) = base + (N-1)*interval
    // Provisional anchor from the sync schedule; re-anchored to the first
    // observed data arrival (see HandleData) so the systematic offset is ~0.
    s.base_recv_ns = recv_ns + (int64_t)(msg.start_delay_ms() * 1000000ULL);
    s.interval_ns  = msg.interval_ms() * 1000000ULL;
    s.anchored     = false;
    s.nextExpected = 1;
    s.maxSeqSeen   = 0;

    // Remember the client address so we can reply even for slots we recover
    // (e.g. via the leader) before ever receiving them directly.
    clientAddr[msg.client_id()] = std::unique_ptr<TransportAddress>(remote.clone());

    Notice("[Replica %d] sync from client %" PRIu64 ": interval=%" PRIu64 "ms",
           replicaIdx, msg.client_id(), msg.interval_ms());
    // Gap detection is armed only once we have an anchor (first data arrival),
    // so we never declare a gap before the stream has actually started.
}

void
PaxosBusReplica::HandleData(const TransportAddress &remote,
                             const ::paxosbus::proto::DataMessage &msg)
{
    auto it = clients.find(msg.client_id());
    if (it == clients.end()) {
        Warning("[Replica %d] data from unsynced client %" PRIu64 ", ignoring",
                replicaIdx, msg.client_id());
        return;
    }
    ClientStream &s = it->second;

    // Always (re)learn the client address from live traffic.
    clientAddr[msg.client_id()] = std::unique_ptr<TransportAddress>(remote.clone());

    s.maxSeqSeen = std::max(s.maxSeqSeen, msg.seq_num());

    // Deterministic drop simulation (gap mode only).
    if (gapEnabled && ShouldDrop(msg.seq_num())) {
        winDrops++;
        Notice("[Replica %d] DROP seq=%" PRIu64 " client=%" PRIu64
               " (simulated loss%s)",
               replicaIdx, msg.seq_num(), msg.client_id(),
               (noopMod > 0 && msg.seq_num() % noopMod == 0) ? ", all replicas"
                                                             : ", this follower");
        return;
    }

    int64_t actual_ns = (int64_t)NowNs();

    // Sliding re-anchor: choose b on every received message so that
    // expected(this seq) == its actual arrival. This tracks the live stream so
    // the gap deadline stays accurate (~ true due time + Delta) even when the
    // client's seq numbering is inflated by resends. Compute the delta against
    // the PRIOR anchor first (so it reflects real jitter), then re-anchor.
    int64_t expected_ns = s.base_recv_ns + (int64_t)((msg.seq_num() - 1) * s.interval_ns);
    int64_t delta_us    = (actual_ns - expected_ns) / 1000;
    s.base_recv_ns = actual_ns - (int64_t)((msg.seq_num() - 1) * s.interval_ns);
    s.anchored = true;

    // Per-message detail goes into the 1-second summary, not one line per seq.
    if (winRecv == 0) {
        winDeltaMinUs = winDeltaMaxUs = delta_us;
    } else {
        winDeltaMinUs = std::min(winDeltaMinUs, delta_us);
        winDeltaMaxUs = std::max(winDeltaMaxUs, delta_us);
    }
    winDeltaSumUs += delta_us;
    winRecv++;

    if (gapEnabled) {
        RecordReceived(msg.client_id(), msg.seq_num(),
                       msg.payload(), msg.send_time_ns());
        ReplyToClient(msg.client_id(), msg.seq_num());
        AdvanceContiguous(msg.client_id());
    } else {
        ReplyToClient(msg.client_id(), msg.seq_num());
    }
}

void
PaxosBusReplica::OnStatsTimer()
{
    if (winRecv || winDrops || winGaps || winRecovered || winNoops) {
        int64_t deltaAvgUs = winRecv ? winDeltaSumUs / (int64_t)winRecv : 0;
        Notice("[Replica %d] 1s: received=%" PRIu64 " dropped=%" PRIu64
               " delta_avg=%+" PRId64 "us delta_min=%+" PRId64 "us delta_max=%+" PRId64 "us"
               " gaps=%" PRIu64 " recovered=%" PRIu64 " noops=%" PRIu64,
               replicaIdx, winRecv, winDrops,
               deltaAvgUs, winDeltaMinUs, winDeltaMaxUs,
               winGaps, winRecovered, winNoops);
        winRecv = winDrops = winGaps = winRecovered = winNoops = 0;
        winDeltaSumUs = winDeltaMinUs = winDeltaMaxUs = 0;
    }
    transport->Timer(1000, [this]() { OnStatsTimer(); });
}

void
PaxosBusReplica::ReplyToClient(uint64_t client_id, uint64_t seq)
{
    auto addr = clientAddr.find(client_id);
    if (addr == clientAddr.end()) {
        Warning("[Replica %d] no address for client %" PRIu64 ", cannot reply",
                replicaIdx, client_id);
        return;
    }

    ::paxosbus::proto::DataReplyMessage reply;
    reply.set_client_id(client_id);
    reply.set_seq_num(seq);
    reply.set_view_id(view_id_);
    reply.set_log_slot_num(seq);
    reply.set_replica_idx((uint32_t)replicaIdx);

    if (!transport->SendMessage(this, *(addr->second), reply)) {
        Warning("[Replica %d] failed to send reply for seq=%" PRIu64, replicaIdx, seq);
    }
}

void
PaxosBusReplica::RecordReceived(uint64_t client_id, uint64_t seq,
                                const std::string &payload, uint64_t send_time_ns)
{
    ClientStream &s = clients[client_id];
    s.maxSeqSeen = std::max(s.maxSeqSeen, seq);
    LogEntry &e = s.log[seq];
    if (e.state == LOG_NOOP) {
        // A committed NoOp is final; never overwrite with data.
        return;
    }
    e.state        = LOG_RECEIVED;
    e.payload      = payload;
    e.send_time_ns = send_time_ns;
    e.recv_ns      = NowNs();
}

void
PaxosBusReplica::AdvanceContiguous(uint64_t client_id)
{
    ClientStream &s = clients[client_id];
    // Walk the frontier forward over any contiguous filled slots. Replies for
    // RECEIVED slots already went out at receipt/recovery time; NoOp slots get
    // no client reply by design.
    while (true) {
        auto it = s.log.find(s.nextExpected);
        if (it == s.log.end() || it->second.state == LOG_EMPTY) {
            break;
        }
        s.nextExpected++;
    }
    if (gapEnabled) {
        ArmGapTimer(client_id);
    }
}

void
PaxosBusReplica::ArmGapTimer(uint64_t client_id)
{
    ClientStream &s = clients[client_id];
    if (s.gapTimerId != 0) {
        transport->CancelTimer(s.gapTimerId);
        s.gapTimerId = 0;
    }
    // Deadline for the frontier slot: expected arrival + Delta.
    int64_t deadline_ns = s.base_recv_ns +
        (int64_t)((s.nextExpected - 1) * s.interval_ns) +
        (int64_t)(deltaMs * 1000000ULL);
    int64_t now_ns = (int64_t)NowNs();
    int64_t ms = (deadline_ns - now_ns) / 1000000;
    if (ms <= 0) {
        // Deadline already passed: this is the rescan cadence while gaps are
        // outstanding. Recheck every Delta; actual GapRequest retries are
        // additionally rate-limited to gapRetryMs per slot.
        ms = (int64_t)deltaMs;
    }
    s.gapTimerId = transport->Timer((uint64_t)ms,
                                    [this, client_id]() { OnGapTimeout(client_id); });
}

void
PaxosBusReplica::OnGapTimeout(uint64_t client_id)
{
    ClientStream &s = clients[client_id];
    s.gapTimerId = 0;  // one-shot fired

    // Parallel gap scan: every EMPTY slot from the frontier up to the highest
    // seq seen whose own T+Delta deadline has passed is a gap; recover them
    // all now instead of serially one frontier slot per round trip. Deadlines
    // increase with slot number, so stop at the first slot still in its window.
    int64_t now_ns = (int64_t)NowNs();
    uint64_t hi = std::max(s.maxSeqSeen, s.nextExpected);
    for (uint64_t slot = s.nextExpected; slot <= hi; slot++) {
        auto it = s.log.find(slot);
        if (it != s.log.end() && it->second.state != LOG_EMPTY) {
            continue;  // already filled (data or NoOp)
        }
        int64_t deadline_ns = s.base_recv_ns +
            (int64_t)((slot - 1) * s.interval_ns) +
            (int64_t)(deltaMs * 1000000ULL);
        if (now_ns < deadline_ns) {
            break;
        }
        if (AmLeader()) {
            // Leader is missing it too -> run gap agreement immediately.
            StartGapAgreement(client_id, slot);
        } else {
            SendGapRequest(client_id, slot);
        }
    }
    AdvanceContiguous(client_id);
}

void
PaxosBusReplica::SendGapRequest(uint64_t client_id, uint64_t seq)
{
    ClientStream &s = clients[client_id];
    uint64_t now = NowNs();

    // Retry backoff: at most one GapRequest per slot per gapRetryMs, so a slow
    // (WAN) leader round trip doesn't trigger a duplicate-request storm.
    auto lit = s.gapLastReqNs.find(seq);
    if (lit != s.gapLastReqNs.end() &&
        now - lit->second < gapRetryMs * 1000000ULL) {
        return;
    }
    s.gapLastReqNs[seq] = now;

    if (s.gapRequestNs.find(seq) == s.gapRequestNs.end()) {
        s.gapRequestNs[seq] = now;
        winGaps++;
        Notice("[Replica %d] GAP detected seq=%" PRIu64 " client=%" PRIu64
               " -> asking leader %d",
               replicaIdx, seq, client_id, config.GetLeaderIndex(view_id_));
    }

    ::paxosbus::proto::GapRequestMessage req;
    req.set_client_id(client_id);
    req.set_seq_num(seq);
    req.set_view_id(view_id_);
    req.set_replica_idx((uint32_t)replicaIdx);

    if (!transport->SendMessageToReplica(this, config.GetLeaderIndex(view_id_), req)) {
        Warning("[Replica %d] failed to send GapRequest for seq=%" PRIu64,
                replicaIdx, seq);
    }
}

void
PaxosBusReplica::HandleGapRequest(const TransportAddress &remote,
                                  const ::paxosbus::proto::GapRequestMessage &msg)
{
    if (!AmLeader()) {
        return;
    }
    ClientStream &s = clients[msg.client_id()];
    auto it = s.log.find(msg.seq_num());

    if (it != s.log.end() && it->second.state == LOG_RECEIVED) {
        // Leader has the data -> serve it.
        ::paxosbus::proto::GapReplyMessage reply;
        reply.set_client_id(msg.client_id());
        reply.set_seq_num(msg.seq_num());
        reply.set_view_id(view_id_);
        reply.set_replica_idx((uint32_t)replicaIdx);
        reply.set_found(true);
        reply.set_send_time_ns(it->second.send_time_ns);
        reply.set_payload(it->second.payload);
        if (!transport->SendMessage(this, remote, reply)) {
            Warning("[Replica %d] failed to send GapReply seq=%" PRIu64,
                    replicaIdx, msg.seq_num());
        }
    } else if (it != s.log.end() && it->second.state == LOG_NOOP) {
        // Already committed as NoOp -> (re)send the GapCommit to the requester.
        ::paxosbus::proto::GapCommitMessage commit;
        commit.set_client_id(msg.client_id());
        commit.set_seq_num(msg.seq_num());
        commit.set_view_id(view_id_);
        if (!transport->SendMessage(this, remote, commit)) {
            Warning("[Replica %d] failed to resend GapCommit seq=%" PRIu64,
                    replicaIdx, msg.seq_num());
        }
    } else {
        // Leader does not have this slot yet. Do NOT escalate to a NoOp here:
        // the message may simply be in flight (a follower's T+Delta can elapse
        // a little before the leader has processed that seq). Ignore the
        // request; the leader will commit a NoOp only when its OWN gap timer
        // fires (i.e. the leader is genuinely missing it past the deadline),
        // and the follower's retry will then be served or NoOp'd.
        return;
    }
}

void
PaxosBusReplica::HandleGapReply(const TransportAddress &remote,
                                const ::paxosbus::proto::GapReplyMessage &msg)
{
    if (!msg.found()) {
        return;
    }
    ClientStream &s = clients[msg.client_id()];
    auto fit = s.log.find(msg.seq_num());
    if (fit != s.log.end() && fit->second.state != LOG_EMPTY) {
        return;  // already resolved
    }

    RecordReceived(msg.client_id(), msg.seq_num(),
                   msg.payload(), msg.send_time_ns());

    uint64_t recovery_us = 0;
    auto rit = s.gapRequestNs.find(msg.seq_num());
    if (rit != s.gapRequestNs.end()) {
        recovery_us = (NowNs() - rit->second) / 1000;
        s.gapRequestNs.erase(rit);
    }
    s.gapLastReqNs.erase(msg.seq_num());
    winRecovered++;
    Notice("[Replica %d] RECOVERED seq=%" PRIu64 " client=%" PRIu64
           " from leader  recovery_latency=%" PRIu64 "us",
           replicaIdx, msg.seq_num(), msg.client_id(), recovery_us);

    ReplyToClient(msg.client_id(), msg.seq_num());
    AdvanceContiguous(msg.client_id());
}

void
PaxosBusReplica::StartGapAgreement(uint64_t client_id, uint64_t seq)
{
    ASSERT(AmLeader());
    ClientStream &s = clients[client_id];

    auto it = s.log.find(seq);
    if (it != s.log.end() && it->second.state == LOG_NOOP) {
        return;  // agreement already done/in progress for this slot
    }

    // Append NoOp at the slot. Leader counts itself; needs f follower acks.
    LogEntry &e = s.log[seq];
    e.state = LOG_NOOP;
    s.noopStartNs[seq] = NowNs();
    s.gapCommitAcks[seq].clear();

    Notice("[Replica %d] NOOP-START slot=%" PRIu64 " client=%" PRIu64,
           replicaIdx, seq, client_id);

    ::paxosbus::proto::GapCommitMessage commit;
    commit.set_client_id(client_id);
    commit.set_seq_num(seq);
    commit.set_view_id(view_id_);
    if (!transport->SendMessageToAll(this, commit)) {
        Warning("[Replica %d] failed to broadcast GapCommit seq=%" PRIu64,
                replicaIdx, seq);
    }
}

void
PaxosBusReplica::HandleGapCommit(const TransportAddress &remote,
                                 const ::paxosbus::proto::GapCommitMessage &msg)
{
    // Followers mark the slot as NoOp (overwriting any data they may hold) and
    // acknowledge the leader for durability counting.
    ClientStream &s = clients[msg.client_id()];
    LogEntry &e = s.log[msg.seq_num()];
    e.state = LOG_NOOP;

    // Any outstanding recovery for this slot is now moot.
    s.gapRequestNs.erase(msg.seq_num());
    s.gapLastReqNs.erase(msg.seq_num());

    Notice("[Replica %d] GapCommit slot=%" PRIu64 " client=%" PRIu64
           " -> marked NOOP",
           replicaIdx, msg.seq_num(), msg.client_id());

    AdvanceContiguous(msg.client_id());

    ::paxosbus::proto::GapCommitReplyMessage reply;
    reply.set_client_id(msg.client_id());
    reply.set_seq_num(msg.seq_num());
    reply.set_view_id(view_id_);
    reply.set_replica_idx((uint32_t)replicaIdx);
    if (!transport->SendMessage(this, remote, reply)) {
        Warning("[Replica %d] failed to send GapCommitReply seq=%" PRIu64,
                replicaIdx, msg.seq_num());
    }
}

void
PaxosBusReplica::HandleGapCommitReply(const TransportAddress &remote,
                                      const ::paxosbus::proto::GapCommitReplyMessage &msg)
{
    if (!AmLeader()) {
        return;
    }
    ClientStream &s = clients[msg.client_id()];
    auto sit = s.noopStartNs.find(msg.seq_num());
    if (sit == s.noopStartNs.end()) {
        return;  // already reached quorum, or not ours
    }

    s.gapCommitAcks[msg.seq_num()].insert(msg.replica_idx());

    // Leader implicitly counts itself: f follower acks => f+1 durable.
    if ((int)s.gapCommitAcks[msg.seq_num()].size() >= config.QuorumSize() - 1) {
        uint64_t noop_us = (NowNs() - sit->second) / 1000;
        winNoops++;
        Notice("[Replica %d] NOOP-DURABLE slot=%" PRIu64 " client=%" PRIu64
               "  noop_latency=%" PRIu64 "us  quorum=%d (f+1)",
               replicaIdx, msg.seq_num(), msg.client_id(), noop_us,
               config.QuorumSize());
        s.noopStartNs.erase(sit);
        s.gapCommitAcks.erase(msg.seq_num());
    }
}

} // namespace paxosbus
} // namespace specpaxos
