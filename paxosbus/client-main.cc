// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
#include "lib/configuration.h"
#include "lib/udptransport.h"
#include "paxosbus/client.h"

#include <unistd.h>
#include <stdlib.h>
#include <fstream>

static void
Usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s -c <config-file> -I <client-id> [-p <interval-ms>]\n"
            "  -c  path to replica config file\n"
            "  -I  client ID (positive integer, unique per client)\n"
            "  -p  message interval in milliseconds (default: 1000)\n",
            prog);
    exit(1);
}

int
main(int argc, char **argv)
{
    const char *configPath = nullptr;
    uint64_t clientid    = 0;
    uint64_t interval_ms = 1000;

    int opt;
    while ((opt = getopt(argc, argv, "c:I:p:")) != -1) {
        switch (opt) {
        case 'c': configPath = optarg; break;
        case 'I':
        {
            char *end;
            clientid = strtoull(optarg, &end, 10);
            if (*end != '\0' || clientid == 0) {
                fprintf(stderr, "-I requires a positive integer\n");
                Usage(argv[0]);
            }
            break;
        }
        case 'p':
        {
            char *end;
            interval_ms = strtoull(optarg, &end, 10);
            if (*end != '\0' || interval_ms == 0) {
                fprintf(stderr, "-p requires a positive integer (milliseconds)\n");
                Usage(argv[0]);
            }
            break;
        }
        default: Usage(argv[0]);
        }
    }

    if (!configPath || clientid == 0) Usage(argv[0]);

    std::ifstream configStream(configPath);
    if (configStream.fail()) {
        fprintf(stderr, "cannot open config: %s\n", configPath);
        return 1;
    }
    specpaxos::Configuration config(configStream);

    UDPTransport transport(0, 0, 0, nullptr);
    specpaxos::paxosbus::PaxosBusClient client(config, &transport, clientid, interval_ms);
    transport.Timer(0, [&]() { client.Start(); });
    transport.Run();
    return 0;
}
