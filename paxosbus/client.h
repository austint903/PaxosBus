// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#ifndef _PAXOSBUS_CLIENT_H_
#define _PAXOSBUS_CLIENT_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "paxosbus/paxosbus-proto.pb.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace specpaxos {
namespace paxosbus {

class PaxosBusClient : public TransportReceiver
{
public:
    PaxosBusClient(const specpaxos::Configuration &config,
                   Transport *transport,
                   uint64_t clientid,
                   uint64_t interval_ms,
                   uint64_t resend_ms = 0,   // 0 disables resend-on-no-quorum
                   const std::string &label = "");
    ~PaxosBusClient() {}

    void ReceiveMessage(const TransportAddress &remote,
                        const string &type,
                        const string &data,
                        void *meta_data) override;

    void Start();

private:
    struct InflightEntry {
        uint64_t sendTimeNs;      // this attempt's send time
        uint64_t firstSendTimeNs; // first attempt's send time (stable across resends)
        uint32_t replicaMask;     // bit i set => replica i has replied
        uint8_t  replyCount;
        uint64_t appReqId;        // stable logical id across resends
        uint32_t attempts;        // 1 on first send, +1 per resend
        int      resendTimerId;   // per-seq quorum timeout (0 = none)
    };

    specpaxos::Configuration config;
    Transport *transport;
    uint64_t clientid;

    // Log-line identity, e.g. "Client 1 asia-east1" (label optional).
    std::string self;
    uint64_t interval_ms;
    uint64_t resend_ms;
    uint64_t seq_num;
    uint64_t app_req_id;        // increments once per logical request

    // open-loop tracking: seq -> per-request state
    std::unordered_map<uint64_t, InflightEntry> inflight;
    int sendTimerId;

    // Open-loop pacing: absolute deadline of the next send. The tick handler
    // catches up past-due sends so timer dispatch latency doesn't erode the
    // achieved rate below 1/interval.
    uint64_t nextSendNs;

    // cumulative latency stats (microseconds)
    uint64_t committedCount;
    uint64_t totalRttUs;
    uint64_t resendCount;

    // 1-second window counters for the periodic summary line
    uint64_t winSent;
    uint64_t winCommitted;
    uint64_t winResends;
    uint64_t winRttSumUs;

    void OnSyncWaitDone();
    void SendTick();
    void SendData(uint64_t seq, uint64_t appReqId,
                  uint64_t firstSendNs, uint32_t attempts);
    void OnResendTimeout(uint64_t seq);
    void OnStatsTimer();
    void HandleDataReply(const TransportAddress &remote,
                         const ::paxosbus::proto::DataReplyMessage &msg);
    static uint64_t NowNs();
};

} // namespace paxosbus
} // namespace specpaxos

#endif  // _PAXOSBUS_CLIENT_H_
