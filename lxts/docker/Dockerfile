FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake build-essential libhiredis-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --parallel $(nproc)

# ── Runtime image ───────────────────────────────────────────
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libhiredis0.14 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/lxts_engine /usr/local/bin/lxts_engine
COPY --from=builder /src/build/bench_latency /usr/local/bin/bench_latency

ENTRYPOINT ["lxts_engine"]
CMD ["--strategy", "momentum", "--symbol", "AAPL"]
