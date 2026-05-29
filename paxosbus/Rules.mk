d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), replica.cc client.cc replica-main.cc client-main.cc)

PROTOS += $(addprefix $(d), paxosbus-proto.proto)

OBJS-paxosbus-client := $(o)client.o $(o)paxosbus-proto.o \
                        $(LIB-message) $(LIB-configuration)

OBJS-paxosbus-replica := $(o)replica.o $(o)paxosbus-proto.o \
                         $(LIB-message) $(LIB-configuration)

$(d)paxosbus-replica: $(o)replica-main.o $(OBJS-paxosbus-replica) $(LIB-udptransport)
$(d)paxosbus-client:  $(o)client-main.o  $(OBJS-paxosbus-client)  $(LIB-udptransport)

BINS += $(d)paxosbus-replica $(d)paxosbus-client
