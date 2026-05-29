// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#ifndef _PAXOSBUS_CLIENT_H_
#define _PAXOSBUS_CLIENT_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "paxosbus/paxosbus-proto.pb.h"
#include "common/quorumset.h"

#include <cstdint>
#include <map>

namespace specpaxos {
namespace paxosbus {

class PaxosBusClient : public TransportReceiver
{
public:
    PaxosBusClient(const specpaxos::Configuration &config,
                   Transport *transport,
                   uint64_t clientid,
                   uint64_t interval_ms);
    ~PaxosBusClient() {}

    void ReceiveMessage(const TransportAddress &remote,
                        const string &type,
                        const string &data,
                        void *meta_data) override;

    void Start();

private:
    specpaxos::Configuration config;
    Transport *transport;
    uint64_t clientid;
    uint64_t interval_ms;
    uint64_t seq_num;

    // quorum state
    QuorumSet<uint64_t, ::paxosbus::proto::DataReplyMessage> replyQuorum;
    bool waitingForQuorum;
    uint64_t pendingSeqNum;
    uint64_t pendingSendTimeNs;

    // rolling latency statistics (microseconds)
    uint64_t committedCount;
    uint64_t totalRttUs;

    void OnSyncWaitDone();
    void SendNextData();
    void HandleDataReply(const TransportAddress &remote,
                         const ::paxosbus::proto::DataReplyMessage &msg);
    static uint64_t NowNs();
};

} // namespace paxosbus
} // namespace specpaxos

#endif  // _PAXOSBUS_CLIENT_H_
