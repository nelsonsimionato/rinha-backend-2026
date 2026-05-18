FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY c-impl/lb/Makefile c-impl/lb/lb.c ./
RUN make clean >/dev/null 2>&1 || true; make

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libc6 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/lb /lb
ENTRYPOINT ["/lb"]
