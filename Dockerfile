FROM golang:1.24-alpine AS builder

WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download

# Copy the entire project
COPY . .

# Generate the O(1) MCC Risk Map
RUN go run tools/mccgen.go

# resources/index.bin must already exist in the build context.
# Run `make index` locally before `docker compose build`; the Makefile's `up` target chains this automatically.
RUN test -s resources/index.bin || (echo "ERROR: resources/index.bin missing or empty. Run 'make index' first." && exit 1)

# Build the main API binary (includes distance_amd64.s + all .go files) and healthcheck
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-w -s" -o api .
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-w -s" -o healthcheck ./cmd/healthcheck

# Stage 2: Microscopic runtime environment
FROM scratch
WORKDIR /app

# Copy the compiled binaries and necessary data assets
COPY --from=builder /app/api .
COPY --from=builder /app/healthcheck .
COPY --from=builder /app/resources/index.bin ./resources/index.bin
COPY --from=builder /app/resources/normalization.json ./resources/normalization.json

# Native Docker Healthcheck targeting the compiled binary instead of curl
HEALTHCHECK --interval=2s --timeout=1s --retries=20 CMD ["./healthcheck"]

# Match Go's thread count to the cgroup CPU quota (0.40 vCPU per container).
# Without this, NumCPU returns the host count (4) and the runtime spins up 4 OS
# threads competing for 0.40 vCPU — each context switch trashes the 45 MB index
# data working set in L2/L3. GOMAXPROCS=1 means one OS thread runs all
# goroutines cooperatively.
ENV GOMAXPROCS=1
# Heap is dominated by the 45 MB read-only index. Memory pressure is low, so
# we can afford to GC less often.
ENV GOGC=200
ENV GOMEMLIMIT=140MiB

EXPOSE 9999
CMD ["./api"]
