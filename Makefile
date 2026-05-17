.PHONY: clean-host generate index up down test all

# Discovers and shuts down external development containers to isolate hardware capacity
clean-host:
	@echo "Shutting down external dev containers to free up hardware..."
	-docker stop $$(docker ps -aq) 2>/dev/null
	-docker rm $$(docker ps -aq) 2>/dev/null

# Generates the static MCC Risk array
generate:
	go run tools/mccgen.go

# Build the feature-hash partition index (format v6) from resources/references.json.gz.
# Splits records into 256 pools by an 8-bit hash of fraud-discriminative dimensions
# (idx 5,9,10,11,12,2). Runtime brute-force scans matching partition + 8 Hamming-1
# neighbors per query. Deterministic, no clustering, no tuning knobs.
resources/index.bin: resources/references.json.gz tools/build_partition_hash.go
	go run tools/build_partition_hash.go

# Alias for the index-bin file target
index: resources/index.bin

# Builds and boots the cluster (rebuilds index.bin if references.json.gz changed)
up: index
	docker compose up --build -d

# Tears down containers and volumes
down:
	docker compose down -v

# Executes the k6 load test
test:
	k6 run k6/test.js

# Sequences the full automated test pipeline natively blocking until healthy
all: clean-host generate index up test
