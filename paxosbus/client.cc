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
      replyQuorum(config.QuorumSize()),
      waitingForQuorum(false), pendingSeqNum(0), pendingSendTimeNs(0),
      committedCount(0), totalRttUs(0)
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
    Notice("[Client %" PRIu64 "] sync wait done, starting data phase (interval=%" PRIu64 "ms)",
           clientid, interval_ms);
    SendNextData();
}

void
PaxosBusClient::SendNextData()
{
    if (waitingForQuorum) {
        Warning("[Client %" PRIu64 "] SendNextData called while already waiting for quorum on seq=%"
                PRIu64 ", ignoring", clientid, pendingSeqNum);
        return;
    }

    ++seq_num;
    pendingSeqNum = seq_num;
    pendingSendTimeNs = NowNs();
    waitingForQuorum = true;
    replyQuorum.Clear();

    ::paxosbus::proto::DataMessage dataMsg;
    dataMsg.set_client_id(clientid);
    dataMsg.set_seq_num(seq_num);
    dataMsg.set_send_time_ns(pendingSendTimeNs);
    dataMsg.set_payload("hello");

    transport->SendMessageToAll(this, dataMsg);
    Notice("[Client %" PRIu64 "] seq=%" PRIu64 " SENT  to %d replicas, waiting for %d (f+1)",
           clientid, seq_num, config.n, config.QuorumSize());
}

void
PaxosBusClient::HandleDataReply(const TransportAddress &remote,
                                 const ::paxosbus::proto::DataReplyMessage &msg)
{
    if (!waitingForQuorum) {
        return;
    }
    if (msg.seq_num() != pendingSeqNum) {
        return;
    }

    auto *msgs = replyQuorum.AddAndCheckForQuorum(
        msg.seq_num(), (int)msg.replica_idx(), msg);

    if (msgs == nullptr) {
        return;
    }

    uint64_t rttUs = (NowNs() - pendingSendTimeNs) / 1000;
    committedCount++;
    totalRttUs += rttUs;
    uint64_t avgRttUs = totalRttUs / committedCount;

    Notice("[Client %" PRIu64 "] seq=%" PRIu64 " COMMITTED"
           "  rtt=%" PRIu64 "us  replies=%d (f+1)"
           "  avg=%" PRIu64 "us  total_committed=%" PRIu64,
           clientid, pendingSeqNum, rttUs, config.QuorumSize(),
           avgRttUs, committedCount);

    waitingForQuorum = false;
    transport->Timer(interval_ms, [this]() { SendNextData(); });
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
