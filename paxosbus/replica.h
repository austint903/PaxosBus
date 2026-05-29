// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#ifndef _PAXOSBUS_REPLICA_H_
#define _PAXOSBUS_REPLICA_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "paxosbus/paxosbus-proto.pb.h"

#include <map>
#include <cstdint>

namespace specpaxos {
namespace paxosbus {

class PaxosBusReplica : public TransportReceiver
{
public:
    PaxosBusReplica(const specpaxos::Configuration &config,
                    int replicaIdx,
                    Transport *transport);
    ~PaxosBusReplica() {}

    void ReceiveMessage(const TransportAddress &remote,
                        const string &type,
                        const string &data,
                        void *meta_data) override;

private:
    struct ClientSyncState {
        int64_t  base_recv_ns;
        uint64_t interval_ns;
        uint64_t next_seq;
    };

    specpaxos::Configuration config;
    int replicaIdx;
    uint64_t view_id_;
    Transport *transport;
    std::map<uint64_t, ClientSyncState> clientStates;

    void HandleSync(const ::paxosbus::proto::SyncMessage &msg);
    void HandleData(const TransportAddress &remote,
                    const ::paxosbus::proto::DataMessage &msg);
    static uint64_t NowNs();
};

} // namespace paxosbus
} // namespace specpaxos

#endif  // _PAXOSBUS_REPLICA_H_
