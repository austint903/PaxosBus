// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#include "paxosbus/client.h"
#include "lib/message.h"

#include <ctime>
#include <cinttypes>

namespace specpaxos {
namespace paxosbus {

PaxosBusClient::PaxosBusClient(const specpaxos::Configuration &config,
                                Transport *transport,
                                uint64_t clientid,
                                uint64_t interval_ms,
                                uint64_t resend_ms,
                                const std::string &label)
    : config(config), transport(transport),
      clientid(clientid), interval_ms(interval_ms), resend_ms(resend_ms),
      seq_num(0), app_req_id(0),
      sendTimerId(0), nextSendNs(0),
      committedCount(0), totalRttUs(0), resendCount(0),
      winSent(0), winCommitted(0), winResends(0), winRttSumUs(0)
{
    self = "Client " + std::to_string(clientid) +
           (label.empty() ? "" : " " + label);
    transport->Register(this, config, -1, -1);
    Notice("[%s] started  interval=%" PRIu64 "ms  replicas=%d  f=%d  quorum=%d (f+1, must include leader)%s",
           self.c_str(), interval_ms, config.n, config.f, config.QuorumSize(),
           resend_ms ? "  resend=on" : "");
    if (resend_ms) {
        Notice("[%s] resend-on-no-quorum timeout=%" PRIu64 "ms",
               self.c_str(), resend_ms);
    }
}

uint64_t
PaxosBusClient::NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void
PaxosBusClient::Start()
{
    ::paxosbus::proto::SyncMessage syncMsg;
    syncMsg.set_client_id(clientid);
    syncMsg.set_send_time_ns(NowNs());
    syncMsg.set_interval_ms(interval_ms);
    syncMsg.set_start_delay_ms(5000);

    transport->SendMessageToAll(this, syncMsg);
    Notice("[%s] sync sent, waiting 5s before data phase", self.c_str());

    transport->Timer(5000, [this]() { OnSyncWaitDone(); });
}

void
PaxosBusClient::OnSyncWaitDone()
{
    Notice("[%s] sync wait done, starting open-loop data phase (interval=%" PRIu64 "ms)",
           self.c_str(), interval_ms);
    nextSendNs = NowNs();
    transport->Timer(1000, [this]() { OnStatsTimer(); });
    SendTick();
}

void
PaxosBusClient::SendTick()
{
    // Each tick is a new logical request occupying the next slot. Send every
    // message whose absolute deadline has passed (catch-up), so late timer
    // dispatch produces a small burst instead of a permanently lower rate.
    uint64_t now = NowNs();
    while (now >= nextSendNs) {
        SendData(++seq_num, ++app_req_id, 0, 1);
        nextSendNs += interval_ms * 1000000ULL;
        now = NowNs();
    }
    uint64_t delay_ms = (nextSendNs - now) / 1000000ULL;
    if (delay_ms == 0) {
        delay_ms = 1;
    }
    sendTimerId = transport->Timer(delay_ms, [this]() { SendTick(); });
}

void
PaxosBusClient::SendData(uint64_t seq, uint64_t appReqId,
                         uint64_t firstSendNs, uint32_t attempts)
{
    uint64_t now = NowNs();
    InflightEntry e{ now, firstSendNs ? firstSendNs : now,
                     0u, 0, appReqId, attempts, 0 };

    ::paxosbus::proto::DataMessage dataMsg;
    dataMsg.set_client_id(clientid);
    dataMsg.set_seq_num(seq);
    dataMsg.set_send_time_ns(now);
    dataMsg.set_payload("hello");
    dataMsg.set_app_req_id(appReqId);

    transport->SendMessageToAll(this, dataMsg);
    winSent++;

    // Arm the per-seq quorum timeout. If the slot is NoOp'd (no quorum), we
    // resend the same logical request at a fresh slot.
    if (resend_ms) {
        e.resendTimerId = transport->Timer(resend_ms,
                                           [this, seq]() { OnResendTimeout(seq); });
    }
    inflight[seq] = e;
}

void
PaxosBusClient::OnResendTimeout(uint64_t seq)
{
    auto it = inflight.find(seq);
    if (it == inflight.end()) {
        return;  // already committed
    }
    uint64_t appReqId    = it->second.appReqId;
    uint64_t firstSendNs = it->second.firstSendTimeNs;
    uint32_t attempts    = it->second.attempts;
    // Abandon the old slot (presumed NoOp'd, or the leader's reply was lost:
    // either way no leader-inclusive quorum) and resend the same logical
    // request at a brand-new slot.
    inflight.erase(it);

    uint64_t newSeq = ++seq_num;
    resendCount++;
    winResends++;
    Notice("[%s] NO-QUORUM seq=%" PRIu64 " app_req=%" PRIu64
           "  resending as seq=%" PRIu64 "  attempt=%u  total_resends=%" PRIu64,
           self.c_str(), seq, appReqId, newSeq, attempts + 1, resendCount);

    SendData(newSeq, appReqId, firstSendNs, attempts + 1);
}

void
PaxosBusClient::OnStatsTimer()
{
    if (winSent || winCommitted || winResends) {
        uint64_t winAvgUs = winCommitted ? winRttSumUs / winCommitted : 0;
        uint64_t cumAvgUs = committedCount ? totalRttUs / committedCount : 0;
        Notice("[%s] 1s: sent=%" PRIu64 " committed=%" PRIu64
               " resends=%" PRIu64 " inflight=%zu rtt_avg=%" PRIu64 "us"
               "  cumulative: committed=%" PRIu64 " rtt_avg=%" PRIu64 "us",
               self.c_str(), winSent, winCommitted, winResends, inflight.size(),
               winAvgUs, committedCount, cumAvgUs);
        winSent = winCommitted = winResends = winRttSumUs = 0;
    }
    transport->Timer(1000, [this]() { OnStatsTimer(); });
}

void
PaxosBusClient::HandleDataReply(const TransportAddress &remote,
                                 const ::paxosbus::proto::DataReplyMessage &msg)
{
    auto it = inflight.find(msg.seq_num());
    if (it == inflight.end()) {
        return;
    }

    uint32_t bit = 1u << msg.replica_idx();
    if (it->second.replicaMask & bit) {
        return;
    }
    it->second.replicaMask |= bit;
    it->second.replyCount++;

    uint64_t now = NowNs();
    // Per-replica RTT measurement line (one per first reply from each replica;
    // run-gcp.sh's summary and analyze-logs.py both parse this format).
    Notice("[%s] REPLY from replica=%u  rtt=%" PRIu64 "us  seq=%" PRIu64,
           self.c_str(), msg.replica_idx(),
           (now - it->second.sendTimeNs) / 1000, msg.seq_num());

    // Commit requires f+1 replies INCLUDING the leader's (as in NOPaxos): the
    // leader's log is authoritative during gap agreement, so a quorum without
    // it could later be overwritten by a NoOp commit.
    int leaderIdx = config.GetLeaderIndex(msg.view_id());
    if (it->second.replyCount < config.QuorumSize() ||
        !(it->second.replicaMask & (1u << leaderIdx))) {
        return;
    }

    uint64_t rttUs   = (now - it->second.sendTimeNs) / 1000;
    uint64_t totalUs = (now - it->second.firstSendTimeNs) / 1000;
    if (it->second.resendTimerId != 0) {
        transport->CancelTimer(it->second.resendTimerId);
    }
    committedCount++;
    totalRttUs += rttUs;
    winCommitted++;
    winRttSumUs += rttUs;

    Notice("[%s] COMMITTED seq=%" PRIu64 " app_req=%" PRIu64
           " rtt=%" PRIu64 "us total=%" PRIu64 "us attempts=%u",
           self.c_str(), msg.seq_num(), it->second.appReqId,
           rttUs, totalUs, it->second.attempts);

    inflight.erase(it);
}

void
PaxosBusClient::ReceiveMessage(const TransportAddress &remote,
                                const string &type,
                                const string &data,
                                void *meta_data)
{
    static ::paxosbus::proto::DataReplyMessage replyMsg;

    if (type == replyMsg.GetTypeName()) {
        replyMsg.ParseFromString(data);
        HandleDataReply(remote, replyMsg);
    }
}

} // namespace paxosbus
} // namespace specpaxos
