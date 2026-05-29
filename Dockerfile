FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    g++ \
    protobuf-compiler \
    pkg-config \
    libunwind-dev \
    libssl-dev \
    libprotobuf-dev \
    libevent-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /nopaxos
COPY . .
RUN make
