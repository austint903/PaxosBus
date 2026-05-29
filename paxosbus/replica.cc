// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#include "paxosbus/replica.h"
#include "lib/message.h"

#include <ctime>
#include <cinttypes>

namespace specpaxos {
namespace paxosbus {

PaxosBusReplica::PaxosBusReplica(const specpaxos::Configuration &config,
                                  int replicaIdx,
                                  Transport *transport)
    : config(config), replicaIdx(replicaIdx), view_id_(0), transport(transport)
{
    transport->Register(this, config, 0, replicaIdx);
    Notice("[Replica %d] started (view=0, f=%d, quorum=%d)",
           replicaIdx, config.f, config.QuorumSize());
}

uint64_t
PaxosBusReplica::NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void
PaxosBusReplica::ReceiveMessage(const TransportAddress &remote,
                                 const string &type,
                                 const string &data,
                                 void *meta_data)
{
    static ::paxosbus::proto::SyncMessage syncMsg;
    static ::paxosbus::proto::DataMessage dataMsg;

    if (type == syncMsg.GetTypeName()) {
        syncMsg.ParseFromString(data);
        HandleSync(syncMsg);
    } else if (type == dataMsg.GetTypeName()) {
        dataMsg.ParseFromString(data);
        HandleData(remote, dataMsg);
    } else {
        Warning("Replica %d: unknown message type %s", replicaIdx, type.c_str());
    }
}

void
PaxosBusReplica::HandleSync(const ::paxosbus::proto::SyncMessage &msg)
{
    int64_t recv_ns = (int64_t)NowNs();
    ClientSyncState &state = clientStates[msg.client_id()];
    // Anchor baseline to when the first data message is expected to arrive:
    // base = sync_recv + start_delay, so expected(N) = base + (N-1)*interval
    state.base_recv_ns = recv_ns + (int64_t)(msg.start_delay_ms() * 1000000ULL);
    state.interval_ns  = msg.interval_ms() * 1000000ULL;
    state.next_seq     = 1;
    Notice("[replica %d] sync from client %" PRIu64 ": interval=%" PRIu64 "ms",
           replicaIdx, msg.client_id(), msg.interval_ms());
}

void
PaxosBusReplica::HandleData(const TransportAddress &remote,
                             const ::paxosbus::proto::DataMessage &msg)
{
    auto it = clientStates.find(msg.client_id());
    if (it == clientStates.end()) {
        Warning("[Replica %d] data from unsynced client %" PRIu64 ", ignoring",
                replicaIdx, msg.client_id());
        return;
    }
    ClientSyncState &state = it->second;
    int64_t actual_ns   = (int64_t)NowNs();
    int64_t expected_ns = state.base_recv_ns + (int64_t)((msg.seq_num() - 1) * state.interval_ns);
    int64_t delta_us    = (actual_ns - expected_ns) / 1000;
    Notice("[Replica %d] seq=%" PRIu64 " from client %" PRIu64
           "  delta=%+" PRId64 "us  view=%" PRIu64 "  slot=%" PRIu64 "  replying",
           replicaIdx, msg.seq_num(), msg.client_id(),
           delta_us, view_id_, msg.seq_num());

    state.next_seq = msg.seq_num() + 1;

    ::paxosbus::proto::DataReplyMessage reply;
    reply.set_client_id(msg.client_id());
    reply.set_seq_num(msg.seq_num());
    reply.set_view_id(view_id_);
    reply.set_log_slot_num(msg.seq_num());
    reply.set_replica_idx((uint32_t)replicaIdx);

    if (!transport->SendMessage(this, remote, reply)) {
        Warning("[Replica %d] failed to send reply for seq=%" PRIu64, replicaIdx, msg.seq_num());
    }
}

} // namespace paxosbus
} // namespace specpaxos
