.PHONY: clean-host generate index up down test all

# Discovers and shuts down external development containers to isolate hardware capacity
clean-host:
	@echo "Shutting down external dev containers to free up hardware..."
	-docker stop $$(docker ps -aq) 2>/dev/null
	-docker rm $$(docker ps -aq) 2>/dev/null

# Generates the static MCC Risk array
generate:
	go run tools/mccgen.go

# Build the IVF index from resources/references.json.gz.
# Defaults: K=2048 centroids, 1 Lloyd iteration. Override with `make index IVF_K=… IVF_ITERS=…`.
IVF_K ?= 2048
IVF_ITERS ?= 1
resources/index.bin: resources/references.json.gz tools/build_ivf.go
	go run tools/build_ivf.go -k $(IVF_K) -iters $(IVF_ITERS)

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
