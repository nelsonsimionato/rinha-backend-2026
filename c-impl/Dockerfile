FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
# Source tree (Makefile + src/) — context is project root; paths relative to it.
COPY c-impl/Makefile ./
COPY c-impl/src/     src/

# Drop any host-side object/build artifacts that may have leaked into the
# context (gcc version mismatch otherwise produces lto bytecode errors).
RUN make clean >/dev/null 2>&1 || true; make -j"$(nproc)"

# Stage 2: scratch runtime. Static binary + immutable index file.
FROM scratch
COPY --from=builder /build/api                       /api
# resources/index.bin is supplied by the build context root (project root); the
# compose file uses `context: ..` to mount the repo. We rely on tools/build_*
# having produced resources/index.bin already.
COPY resources/index.bin                             /resources/index.bin

HEALTHCHECK --interval=2s --timeout=1s --retries=20 CMD ["/api", "--healthcheck"]

EXPOSE 8080
ENTRYPOINT ["/api"]
