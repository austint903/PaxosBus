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
    fprintf(stderr, "usage: %s -c <config-file> -i <replica-index>\n", prog);
    exit(1);
}

int
main(int argc, char **argv)
{
    const char *configPath = nullptr;
    int index = -1;

    int opt;
    while ((opt = getopt(argc, argv, "c:i:")) != -1) {
        switch (opt) {
        case 'c': configPath = optarg; break;
        case 'i':
        {
            char *end;
            index = (int)strtol(optarg, &end, 10);
            if (*end != '\0' || index < 0) Usage(argv[0]);
            break;
        }
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
    specpaxos::paxosbus::PaxosBusReplica replica(config, index, &transport);
    transport.Run();
    return 0;
}
