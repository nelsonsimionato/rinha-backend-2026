.PHONY: clean-host generate index up down test all

# Discovers and shuts down external development containers to isolate hardware capacity
clean-host:
	@echo "Shutting down external dev containers to free up hardware..."
	-docker stop $$(docker ps -aq) 2>/dev/null
	-docker rm $$(docker ps -aq) 2>/dev/null

# Generates the static MCC Risk array
generate:
	go run tools/mccgen.go

# Build the balanced KD-tree index (format v12) from resources/references.json.gz.
# Sliding-midpoint splits on the widest-spread dim, leaf size 32; each node stores
# its subtree AABB so the C runtime prunes exactly with the AVX2 bound_dist_sq kernel.
# Exact 5-NN recall, ~244 records scanned/query (was ~76K with the feature-hash
# partition in build_partition_hash.go, kept for reference).
resources/index.bin: resources/references.json.gz tools/build_kdtree.go
	go run tools/build_kdtree.go

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
