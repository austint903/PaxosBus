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
                                uint64_t interval_ms)
    : config(config), transport(transport),
      clientid(clientid), interval_ms(interval_ms), seq_num(0),
      sendTimerId(0), committedCount(0), totalRttUs(0)
{
    transport->Register(this, config, -1, -1);
    Notice("[Client %" PRIu64 "] started  interval=%" PRIu64 "ms  replicas=%d  f=%d  quorum=%d (f+1)",
           clientid, interval_ms, config.n, config.f, config.QuorumSize());
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
    Notice("Client %" PRIu64 ": sync sent, waiting 5s before data phase", clientid);

    transport->Timer(5000, [this]() { OnSyncWaitDone(); });
}

void
PaxosBusClient::OnSyncWaitDone()
{
    Notice("[Client %" PRIu64 "] sync wait done, starting open-loop data phase (interval=%" PRIu64 "ms)",
           clientid, interval_ms);
    SendTick();
}

void
PaxosBusClient::SendTick()
{
    ++seq_num;
    uint64_t now = NowNs();
    inflight[seq_num] = InflightEntry{ now, 0u, 0 };

    ::paxosbus::proto::DataMessage dataMsg;
    dataMsg.set_client_id(clientid);
    dataMsg.set_seq_num(seq_num);
    dataMsg.set_send_time_ns(now);
    dataMsg.set_payload("hello");

    transport->SendMessageToAll(this, dataMsg);

    sendTimerId = transport->Timer(interval_ms, [this]() { SendTick(); });
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

    if (it->second.replyCount < config.QuorumSize()) {
        return;
    }

    uint64_t rttUs = (NowNs() - it->second.sendTimeNs) / 1000;
    committedCount++;
    totalRttUs += rttUs;
    uint64_t avgRttUs = totalRttUs / committedCount;
    size_t inflightAfter = inflight.size() - 1;

    Notice("[Client %" PRIu64 "] seq=%" PRIu64 " COMMITTED"
           "  rtt=%" PRIu64 "us  replies=%d (f+1)"
           "  avg=%" PRIu64 "us  total_committed=%" PRIu64
           "  inflight=%zu",
           clientid, msg.seq_num(), rttUs, config.QuorumSize(),
           avgRttUs, committedCount, inflightAfter);

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
