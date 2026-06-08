// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#ifndef _PAXOSBUS_CLIENT_H_
#define _PAXOSBUS_CLIENT_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "paxosbus/paxosbus-proto.pb.h"

#include <cstdint>
#include <unordered_map>

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
    struct InflightEntry {
        uint64_t sendTimeNs;
        uint32_t replicaMask;   // bit i set => replica i has replied
        uint8_t  replyCount;
    };

    specpaxos::Configuration config;
    Transport *transport;
    uint64_t clientid;
    uint64_t interval_ms;
    uint64_t seq_num;

    // open-loop tracking: seq -> per-request state
    std::unordered_map<uint64_t, InflightEntry> inflight;
    int sendTimerId;

    // rolling latency stats (microseconds)
    uint64_t committedCount;
    uint64_t totalRttUs;

    void OnSyncWaitDone();
    void SendTick();
    void HandleDataReply(const TransportAddress &remote,
                         const ::paxosbus::proto::DataReplyMessage &msg);
    static uint64_t NowNs();
};

} // namespace paxosbus
} // namespace specpaxos

#endif  // _PAXOSBUS_CLIENT_H_
