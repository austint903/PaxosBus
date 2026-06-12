// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#ifndef _PAXOSBUS_REPLICA_H_
#define _PAXOSBUS_REPLICA_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "paxosbus/paxosbus-proto.pb.h"

#include <map>
#include <set>
#include <memory>
#include <string>
#include <cstdint>

namespace specpaxos {
namespace paxosbus {

class PaxosBusReplica : public TransportReceiver
{
public:
    PaxosBusReplica(const specpaxos::Configuration &config,
                    int replicaIdx,
                    Transport *transport,
                    bool gapEnabled = false,
                    uint64_t dropMod = 2,
                    uint64_t noopMod = 0,
                    uint64_t deltaMs = 10,
                    uint64_t gapRetryMs = 100,
                    const std::string &label = "");
    ~PaxosBusReplica();

    void ReceiveMessage(const TransportAddress &remote,
                        const string &type,
                        const string &data,
                        void *meta_data) override;

private:
    enum LogState { LOG_EMPTY = 0, LOG_RECEIVED, LOG_NOOP };

    struct LogEntry {
        LogState state;
        std::string payload;
        uint64_t   send_time_ns;
        uint64_t   recv_ns;
    };

    struct ClientStream {
        // Linear model: expected arrival of seq N is base_recv_ns + (N-1)*interval_ns.
        // base_recv_ns is re-anchored to the first observed arrival so the
        // systematic sync offset doesn't make the gap detector fire early.
        int64_t  base_recv_ns;
        uint64_t interval_ns;
        bool     anchored;

        // Per-slot log (slot == seq_num). nextExpected is the lowest slot not yet
        // RECEIVED or NOOP (the contiguity frontier the gap detector watches).
        // maxSeqSeen bounds the parallel gap scan: every EMPTY slot in
        // [nextExpected, maxSeqSeen] whose own T+Delta deadline has passed is a gap.
        std::map<uint64_t, LogEntry> log;
        uint64_t nextExpected;
        uint64_t maxSeqSeen;

        // One re-armable one-shot gap timer per client, watching nextExpected's
        // deadline. 0 = none (raw transport->Timer id, not the Timeout class,
        // which auto-repeats and forbids SetTimeout while active).
        int gapTimerId;

        // Follower recovery latency: when we FIRST asked the leader for a slot.
        std::map<uint64_t, uint64_t> gapRequestNs;
        // Retry backoff: when we LAST asked, so duplicates go out at most every
        // gapRetryMs (>= leader RTT) instead of every Delta.
        std::map<uint64_t, uint64_t> gapLastReqNs;

        // Leader NoOp agreement: acks per slot + when agreement started.
        std::map<uint64_t, std::set<uint32_t> > gapCommitAcks;
        std::map<uint64_t, uint64_t>            noopStartNs;

        ClientStream() : base_recv_ns(0), interval_ns(0), anchored(false),
                         nextExpected(1), maxSeqSeen(0), gapTimerId(0) {}
    };

    specpaxos::Configuration config;
    int replicaIdx;
    uint64_t view_id_;
    Transport *transport;

    // Log-line identity, e.g. "Replica 1 europe-north1" (label optional).
    std::string self;

    // Gap-mode configuration.
    bool     gapEnabled;
    uint64_t dropMod;    // non-leaders drop (seq+idx) % dropMod == 0 (recover-from-leader)
    uint64_t noopMod;    // all replicas drop seq % noopMod == 0 (NoOp path); 0 = off
    uint64_t deltaMs;    // confidence interval Delta added to predicted arrival
    uint64_t gapRetryMs; // min spacing between GapRequest retries for one slot

    std::map<uint64_t, ClientStream> clients;
    std::map<uint64_t, std::unique_ptr<TransportAddress> > clientAddr;

    // 1-second window counters for the periodic summary line (all clients).
    uint64_t winRecv;
    uint64_t winDrops;
    uint64_t winGaps;
    uint64_t winRecovered;
    uint64_t winNoops;
    int64_t  winDeltaSumUs;
    int64_t  winDeltaMinUs;
    int64_t  winDeltaMaxUs;

    bool AmLeader() const { return config.GetLeaderIndex(view_id_) == replicaIdx; }
    bool ShouldDrop(uint64_t seq) const;

    void HandleSync(const TransportAddress &remote,
                    const ::paxosbus::proto::SyncMessage &msg);
    void HandleData(const TransportAddress &remote,
                    const ::paxosbus::proto::DataMessage &msg);
    void HandleGapRequest(const TransportAddress &remote,
                          const ::paxosbus::proto::GapRequestMessage &msg);
    void HandleGapReply(const TransportAddress &remote,
                        const ::paxosbus::proto::GapReplyMessage &msg);
    void HandleGapCommit(const TransportAddress &remote,
                         const ::paxosbus::proto::GapCommitMessage &msg);
    void HandleGapCommitReply(const TransportAddress &remote,
                              const ::paxosbus::proto::GapCommitReplyMessage &msg);

    // Slot/log helpers (gap mode).
    void ReplyToClient(uint64_t client_id, uint64_t seq);
    void RecordReceived(uint64_t client_id, uint64_t seq,
                        const std::string &payload, uint64_t send_time_ns);
    void AdvanceContiguous(uint64_t client_id);
    void ArmGapTimer(uint64_t client_id);
    void OnGapTimeout(uint64_t client_id);
    void OnStatsTimer();

    void SendGapRequest(uint64_t client_id, uint64_t seq);
    void StartGapAgreement(uint64_t client_id, uint64_t seq);

    static uint64_t NowNs();
};

} // namespace paxosbus
} // namespace specpaxos

#endif  // _PAXOSBUS_REPLICA_H_
