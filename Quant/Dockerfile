# ── Stage 1: Build ────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build g++ git ca-certificates \
    libcurl4-openssl-dev \
    libzmq3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Quant/ .

RUN cmake -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -B build \
        -S . \
    && cmake --build build --target quant_trader -j$(nproc)

# ── Stage 2: Runtime ──────────────────────────────────────────────────────────
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Asia/Seoul

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 \
    libzmq5 \
    tzdata \
    ca-certificates \
    && ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
    && echo $TZ > /etc/timezone \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/quant_trader .

# config/config.json 은 볼륨 마운트로 주입 (인증정보 포함, 이미지에 미포함)
VOLUME ["/app/config"]

# ZMQ PUB(5555) / ZMQ REP(5556)
EXPOSE 5555 5556

CMD ["./quant_trader", "config/config.json"]
