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
            "usage: %s -c <config-file> -I <client-id> [-p <interval-ms>] [-t <resend-ms>] [-l <label>]\n"
            "  -c  path to replica config file\n"
            "  -I  client ID (positive integer, unique per client)\n"
            "  -p  message interval in milliseconds (default: 1)\n"
            "  -t  resend-on-no-quorum timeout in ms (default: 0 = disabled)\n"
            "  -l  location label shown in every log line, e.g. asia-east1\n",
            prog);
    exit(1);
}

int
main(int argc, char **argv)
{
    const char *configPath = nullptr;
    uint64_t clientid    = 0;
    uint64_t interval_ms = 1;
    uint64_t resend_ms   = 0;
    const char *label    = "";

    int opt;
    while ((opt = getopt(argc, argv, "c:I:p:t:l:")) != -1) {
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
        case 't':
        {
            char *end;
            resend_ms = strtoull(optarg, &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "-t requires a non-negative integer (milliseconds)\n");
                Usage(argv[0]);
            }
            break;
        }
        case 'l': label = optarg; break;
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
    specpaxos::paxosbus::PaxosBusClient client(config, &transport, clientid,
                                               interval_ms, resend_ms, label);
    transport.Timer(0, [&]() { client.Start(); });
    transport.Run();
    return 0;
}
