// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#include "lib/configuration.h"
#include "lib/udptransport.h"
#include "paxosbus/replica.h"

#include <unistd.h>
#include <stdlib.h>
#include <fstream>

static void
Usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s -c <config-file> -i <replica-index>"
            " [-g] [-d <drop-mod>] [-N <noop-mod>] [-D <delta-ms>] [-R <retry-ms>]\n"
            "  -g            enable gap-agreement mode (default: normal processing)\n"
            "  -d <mod>      non-leaders drop (seq+idx) %% mod == 0 (recover-from-leader; default 2)\n"
            "  -N <mod>      ALL replicas drop seq %% mod == 0 (NoOp path; default 0 = off)\n"
            "  -D <ms>       gap-detection confidence interval Delta in ms (default 10)\n"
            "  -R <ms>       min spacing between GapRequest retries per slot (default 100)\n",
            prog);
    exit(1);
}

int
main(int argc, char **argv)
{
    const char *configPath = nullptr;
    int index = -1;
    bool gapEnabled = false;
    uint64_t dropMod = 2;
    uint64_t noopMod = 0;
    uint64_t deltaMs = 10;
    uint64_t gapRetryMs = 100;

    int opt;
    while ((opt = getopt(argc, argv, "c:i:gd:N:D:R:")) != -1) {
        switch (opt) {
        case 'c': configPath = optarg; break;
        case 'i':
        {
            char *end;
            index = (int)strtol(optarg, &end, 10);
            if (*end != '\0' || index < 0) Usage(argv[0]);
            break;
        }
        case 'g': gapEnabled = true; break;
        case 'd': dropMod = strtoull(optarg, nullptr, 10); break;
        case 'N': noopMod = strtoull(optarg, nullptr, 10); break;
        case 'D': deltaMs = strtoull(optarg, nullptr, 10); break;
        case 'R': gapRetryMs = strtoull(optarg, nullptr, 10); break;
        default: Usage(argv[0]);
        }
    }

    if (!configPath || index < 0) Usage(argv[0]);

    std::ifstream configStream(configPath);
    if (configStream.fail()) {
        fprintf(stderr, "cannot open config: %s\n", configPath);
        return 1;
    }
    specpaxos::Configuration config(configStream);

    if (index >= config.n) {
        fprintf(stderr, "replica index %d out of range (n=%d)\n", index, config.n);
        return 1;
    }

    UDPTransport transport(0, 0, 0, nullptr);
    specpaxos::paxosbus::PaxosBusReplica replica(config, index, &transport,
                                                 gapEnabled, dropMod, noopMod,
                                                 deltaMs, gapRetryMs);
    transport.Run();
    return 0;
}
