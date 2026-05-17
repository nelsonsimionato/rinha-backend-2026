# AUDIT_FINDINGS — Rinha 2026 Fraud Detection

Audit of the current implementation in this repository against the spec in `docs/`. Methodology: static reading of `main.go`, `tools/build_hnsw.go`, `Dockerfile`, `docker-compose.yml`, `haproxy.cfg`, plus a paper-trace of the two canonical example payloads from `docs/DETECTION_RULES.md` through the runtime vectorization code, plus a Docker build + container start to confirm the build pipeline succeeds end-to-end on the current 1-record stub.

**Top-line verdict:** the stack builds and serves HTTP 200, but the runtime vectorization writes the wrong fields to the wrong vector indices (C1), the index builder silently drops every fraud label (C3), the index file structure mismatches the runtime's read shape at full scale (C5), the graph construction is not actually HNSW (C6), and the real 3M-vector dataset is absent (C4). End-to-end smoke against spec-shaped payloads returns `{"approved":true,"fraud_score":0}` for both the canonical legit and the canonical fraud examples — i.e. the API is currently a constant function. The submission is not viable until at least C1, C3, C4, C5, C6 are addressed.

---

## CRITICAL findings

### C1. Vectorization writes wrong fields to wrong indices — `main.go:200–240`

**Paper trace of the canonical legit payload** from `docs/DETECTION_RULES.md` (tx-1329056812: amount 41.12, installments 2, requested_at 2026-03-11T18:45:53Z [Wed], customer.avg_amount 82.24, tx_count_24h 3, known_merchants includes MERC-016, merchant.id MERC-016, merchant.mcc 5411, merchant.avg_amount 60.25, terminal.is_online false, terminal.card_present true, terminal.km_from_home 29.23, last_transaction null):

| Idx | Spec dimension          | Spec formula → expected value      | Current code does                                          | Match |
|-----|-------------------------|-------------------------------------|------------------------------------------------------------|-------|
| 0   | `amount`                | 41.12/10000 = 0.0041                | `tx.amount/10000`                                           | ✓     |
| 1   | `installments`          | 2/12 = 0.1667                       | `tx.installments/12`                                        | ✓     |
| 2   | `amount_vs_avg`         | (41.12/82.24)/10 = 0.05             | writes `cus.tx_count_24h/20` = 0.15                         | ✗ wrong field |
| 3   | `hour_of_day`           | 18/23 = 0.7826                      | writes `mer.avg_amount/10000` = 0.006                       | ✗ wrong field |
| 4   | `day_of_week`           | 2/6 = 0.3333 (Wed)                  | writes `(tx.amount/mer.avg_amount)/10` ≈ 0.068              | ✗ wrong field |
| 5   | `minutes_since_last_tx` | -1 (null last_transaction)          | writes literal 0; non-null path uses `last_tx.amount/10000` | ✗ wrong field |
| 6   | `km_from_last_tx`       | -1 (null last_transaction)          | writes literal 0; non-null path uses `last_tx.minutes_since/1440` | ✗ wrong field |
| 7   | `km_from_home`          | 29.23/1000 = 0.0292                 | **never written** → 0                                       | ✗ missing |
| 8   | `tx_count_24h`          | 3/20 = 0.15                         | **never written** → 0                                       | ✗ missing |
| 9   | `is_online`             | 0 (false)                           | **never written** → 0                                       | ✗ accidentally correct only because false maps to 0 |
| 10  | `card_present`          | 1 (true)                            | writes `term.km_from_home/1000` = 0.0292                    | ✗ wrong field |
| 11  | `unknown_merchant`      | 0 (MERC-016 ∈ known_merchants)      | **never written** → 0                                       | ✗ accidentally correct |
| 12  | `mcc_risk`              | mcc_risk["5411"] = 0.15             | **never written** → 0                                       | ✗ missing |
| 13  | `merchant_avg_amount`   | 60.25/10000 = 0.006                 | writes `MccRisk[5411]` = 0.15                               | ✗ wrong field |

**Net:** only indices 0 and 1 are structurally correct. Indices 9 and 11 happen to coincide with the spec output for this particular payload (false→0, known→0) but the code never reads `terminal.is_online` or `customer.known_merchants` at all, so any payload that flips those bits will produce the wrong vector. Five indices (7, 8, 9, 11, 12) are never written and remain at their `uint8` zero value.

The fraud canonical example reproduces the same pattern at higher magnitude (expected `[0.9506, 0.8333, 1.0, 0.2174, 0.8333, -1, -1, 0.9523, 1.0, 0, 1, 1, 0.75, 0.0055]`); the trace omitted here but identical in shape.

Also: `transaction.requested_at` is never parsed in the runtime — there is no `time.Parse` call anywhere in `main.go`. Indices 3 and 4 (hour, day-of-week) cannot be computed without it.

**Required reads from the payload that the code currently ignores:** `transaction.requested_at`, `customer.avg_amount`, `customer.known_merchants`, `merchant.id`, `terminal.is_online`, `terminal.card_present`, `last_transaction.timestamp`, `last_transaction.km_from_current`.

### C2. `last_transaction` null handling — `main.go:234–240`, `tools/build_hnsw.go:26–33`

Spec wording: indices 5 and 6 receive the **literal `-1`** when `last_transaction` is null; this is the only place a vector value escapes [0.0, 1.0].

The codebase implicitly redefines the convention: it quantizes 14-D floats to `uint8` with `clampQuantize`, which maps `x < 0` to byte `0` and the normalized range [0, 1] to bytes [1, 255]. Both the builder and the runtime cooperate on this: the builder sees `-1.0` in the dataset → writes byte 0; the runtime sees `last_transaction == null` → writes byte 0 directly (`vec[5] = 0` at `main.go:235`). The two sides agree, so this is internally consistent.

But it is fragile in two ways: (a) the convention is undocumented and any code change on either side must keep the encoding in sync; (b) byte 0 also encodes "input was below 0 after normalization" for any of the other 12 indices — i.e. the sentinel space is shared with legitimate negative-clamp results, which can happen on indices 7–8 if input values are negative (shouldn't be, but is not asserted). Low-probability risk, but worth a comment.

Note this finding rides on C1: even with the encoding correct, the bytes are being written to the wrong positions.

### C3. Builder silently drops every fraud label — `tools/build_hnsw.go:95–108`

Builder decodes:
```go
var row struct {
    Vector  [Dimensions]float32 `json:"vector"`
    IsFraud bool                `json:"is_fraud"`
}
```

Spec record (`docs/DATASET.md`):
```json
{ "vector": [...14 floats...], "label": "fraud" }
```

The decoder matches `vector` fine and ignores the unknown `label` field. `IsFraud` stays at its zero value `false` for every row. Every record becomes legit; the resulting `hnsw_index.bin` carries zero fraud labels regardless of input.

Empirical confirmation: with the current stub (which happens to use `is_fraud: false`), the runtime returns `fraud_score: 0` for the canonical fraud payload as well as the legit one — `curl` test below.

```
$ curl -X POST http://172.17.0.2:8080/fraud-score \
       -H 'Content-Type: application/json' \
       -d '{...canonical fraud payload from DETECTION_RULES.md...}'
{"approved":true,"fraud_score":0.0000} [HTTP 200]
```

On a correctly-formatted spec dataset the builder would *still* output all-legit (because the field name and type would both fail to bind to the spec's `"label":"fraud"|"legit"` string).

Fix scope: the struct needs `Label string \`json:"label"\`` plus `row.Label == "fraud"` test, or a custom unmarshaler.

### C4. Real `references.json.gz` is absent — `resources/references.json.gz` is 64 bytes / 1 stub record

Spec requires 3,000,000 labeled vectors (~16 MB gzipped, ~284 MB uncompressed). The repo's `resources/references.json.gz` contains a single synthetic record (`vector` of mostly `0.5`s, `is_fraud: false`). Without the real file, none of the verification protocol's runtime measurements (recall, p99 under load, memory hold) can be performed against representative data. Source of the real file is not in the repo and not in `docs/`.

### C5. Builder/runtime HnswM mismatch — `.env:1` (`HNSW_M=16`) vs `tools/build_hnsw.go:17` (`MaxNeighbors = 8`)

The runtime reads `HnswM` from env (default 8) at `main.go:262–266`; the builder hardcodes `MaxNeighbors = 8`. The `hnsw_index.bin` file's graph section is sized `totalPoints * 8 * sizeof(int32)`. At startup the runtime computes `graphLen = totalPoints * HnswM` and slices `binData[offset : offset+graphLen*4]`.

With the current `.env` value `HNSW_M=16`, the requested slice is twice as long as the actual data in the file:
- For the 1-record stub the file is ~59 bytes; the slice would request through byte 91. **No panic was observed** in the docker smoke test because `os.ReadFile`'s underlying buffer rounds up capacity, so a small over-read happens to fit within the allocation. The bytes past the file content are zeros and the search loop sees `graph[i] == 0` (a valid index) — not `-1` — so on a real graph this would walk into bogus neighbors.
- For a 3M-record file (~141 MB), the requested slice (`binData[27 + 42_000_000 + 3_000_000 : … + 192_000_000]`) cannot be satisfied by any plausible buffer allocation: panic on the first slice expression at startup. **Startup-fatal at full scale.**

The smoke build with the 1-record stub does not surface this; it will surface immediately when a real index is loaded.

Fix scope: either remove `HNSW_M` from `.env` (default 8 then matches builder), or parameterize the builder via shared constant / env var and rebuild.

### C6. `build_hnsw.go` does not build HNSW — `tools/build_hnsw.go:117–145`

The construction loop:
```go
for i := int32(1); i < totalPoints; i++ {
    // greedy descent from node 0 to nearest
    // ...
    addHNSWEdge(i, curr)
    addHNSWEdge(curr, i)
}
```

This is a single greedy descent from node 0, adding exactly two undirected edges per inserted point. There is no hierarchical level assignment, no `efConstruction`-bounded candidate exploration, no neighbor-selection heuristic (e.g. heuristic-prune-by-Lid as in the HNSW paper), no diversification. The resulting graph is a near-spanning-tree with ~2-edge fan-out. Recall vs exact 5-NN on this graph will be far below the level needed to stay under the 15% failure cutoff.

This is the dominant correctness bug after C1+C3 — even with a perfect 3M-vector index and a perfect runtime vectorization, fraud detection accuracy would still be very low because the graph cannot guide a query to its true neighbors.

Fix paths (defer to follow-up plan; not part of this audit):
1. Replace with a real HNSW (pure-Go library, since `CGO_ENABLED=0` in Dockerfile rules out C-binding libs)
2. Replace with IVF / VP-Tree (different trade-offs)
3. Accept brute force and optimize the linear scan (memory-bandwidth limited; needs SIMD + sharding to fit p99)

### C7. `SearchHNSW` algorithmic issues — `main.go:112–171`

Findings, in approximate impact order:
- **O(N) pop on the candidate queue** at `main.go:131–133`: `for i := 0; i < state.Candidates.Count; i++ { state.Candidates.Items[i] = state.Candidates.Items[i+1] }`. With `efConstruction` (64) candidates this is up to 64 shifts per popped candidate. A proper min-heap is O(log n).
- The candidate ceiling is the build-time parameter `HnswEfConstruction`, not a separate `ef_search`. Search dynamic-list size should be tunable independently — typical practice is `ef_search ≥ k`, often 50–200.
- Visited bitset is zeroed `for i := range state.Visited { state.Visited[i] = 0 }` on every request — `(3M/64)+1 ≈ 47K` word writes per query. Replace with epoch/generation counter to make it O(1).
- The candidate-removal-after-pop and visited-bit-set-before-distance-check are interleaved correctly, but the termination condition `c.Dist > Q.Items[Count-1].Dist` should be checked *before* the pop and after fetching the smallest candidate; current ordering happens to be correct but is unidiomatic.

These rides on top of C6 — they only matter once the graph is well-formed.

### C8. `example-payloads.json` does not match spec — `resources/example-payloads.json`

Payload shape used in the file:
```json
{ "transaction": { "amount":…, "installments":…, "time":… },
  "customer":    { "tx_count_24h":…, "minutes":… },
  "merchant":    { "mcc":…, "merchant_avg_amount":…, "amount_vs_avg_ratio":… },
  "terminal":    { "km":… },
  "last_transaction": { "amount":…, "km":…, "minutes":… } }
```

Spec shape (`docs/API.md`):
```json
{ "id":…, "transaction": { "amount":…, "installments":…, "requested_at":… },
  "customer": { "avg_amount":…, "tx_count_24h":…, "known_merchants":[…] },
  "merchant": { "id":…, "mcc":…, "avg_amount":… },
  "terminal": { "is_online":…, "card_present":…, "km_from_home":… },
  "last_transaction": null | { "timestamp":…, "km_from_current":… } }
```

`k6/test.js` drives load tests from `example-payloads.json`, so `make test` measures latency against a non-spec-shape payload. Even after C1 is fixed in `main.go`, k6 would still exercise the wrong codepath. Regenerate the payloads from the spec (and use them to validate C1 fix once applied).

---

## HIGH findings

### H1. Quantization precision (uint8) may degrade recall

Vectors are quantized to `uint8` (255 buckets per dimension) for memory savings (42 MB vs 168 MB float32) and SIMD-friendly int distance. Risk: the test labels are produced by exact Euclidean over the *original* floats, so quantization can flip nearest-neighbor ordering near class boundaries. With 3M vectors over a 14-dim hypercube, quantization bucket density is ~0.39 points per (1/255)¹⁴ cell — very sparse, so collisions are rare, but boundary cases will misrank. Recall@5 must be measured against exact float32 k-NN on at least a 100K-row slice; if drop > ~5%, revert to float32.

### H2. `normalization.json` is absent — `resources/`

Spec directs implementations to load constants from `normalization.json`. The runtime currently hardcodes the constants (`10000`, `12`, `1440`, `1000`, `20`, `10000`). The hardcoded values match what the spec lists, but the test harness *could* swap the file. Defensive fix: load the file at startup, fall back to hardcoded defaults if absent. Cheap to add.

### H3. HAProxy `nbthread 2` under 0.20 vCPU — `haproxy.cfg:2`

Two threads competing for 0.20 vCPU adds context-switch jitter at the p99 tail. Switch to `nbthread 1` for predictable latency on the small CPU budget.

### H4. Container healthcheck targets `:8080`, not `:9999`

`cmd/healthcheck/main.go:14` polls `http://localhost:8080/ready` — correct for the *backend* container (each API serves 8080 internally), but worth a note: the *external* readiness contract (`:9999`) is HAProxy's, and HAProxy currently does TCP-only backend checks (`server … check inter 2s rise 3 fall 5` in `haproxy.cfg:21–22`), not HTTP. TCP checks pass as soon as the listener binds, which is *before* the HNSW index finishes pre-warm. Under load right after startup, p99 will see a cold cache on the first batch. Switch HAProxy backend checks to HTTP `/ready` so the LB doesn't route traffic until the API's pre-warm is done.

---

## MEDIUM findings

- **M1.** Visited bitset zeroed per request — see C7 third bullet. Generation-counter trick replaces the memset.
- **M2.** Response formatting via `fmt.Fprintf` on the hot path (`main.go:255`). At sub-millisecond p99 this matters. Possible fraud_scores are exactly `{0/5, 1/5, 2/5, 3/5, 4/5, 5/5}` — six precomputed JSON literals would zero out the format cost.
- **M3.** `reuseport.Listen` (`main.go:316`) provides no benefit with one Go process per container. `net.Listen` is simpler and equivalent here.
- **M4.** Wrong method on each endpoint returns 404 (`main.go:185–258`). Spec doesn't mandate 405 but it's cheaper to be correct.
- **M5.** Stale build artifacts in repo root (`api`, `main`, `rinha_2026`, ~8.2 MB each). Dockerfile rebuilds, so these are unused; add to `.gitignore` and `rm` from the repo to keep image-build context small.

---

## LOW findings

- **L1.** `docker-compose.yml:38,56` comments mention "VP-Tree construction" — stale (code uses HNSW). Edit or delete.
- **L2.** Building the HNSW inside the Docker build (`Dockerfile:14`) means a `docker build` for a real 3M-vector dataset will take build-time CPU minutes (or longer with a corrected HNSW algorithm). Consider pre-building `hnsw_index.bin` outside Docker and `COPY` it in to keep CI/build cheap.
- **L3.** `.env` is committed to the repo (no `.env.example` indirection). Fine here (no secrets), unusual convention.

---

## Verification protocol — execution log

The plan's verification protocol was executed to the extent the absent dataset and audit-only scope permit:

1. **Vectorization parity (paper trace)** — Executed. Confirms C1 on both canonical examples (`docs/DETECTION_RULES.md`). See C1 table.

2. **Builder schema parity** — Executed by static reading + the empirical curl test under C3. The current stub uses the builder's `is_fraud` field, so the stub itself parses; on a spec-format file (`label: "fraud"|"legit"`) the builder would silently produce all-legit labels.

3. **Recall benchmark** — **Not performed.** Requires the real 3M-record `references.json.gz`. Blocker.

4. **End-to-end smoke** — Executed. `docker compose build api1` succeeds (build log shows the four pipeline steps complete in ~22 s wall-clock). `docker run rinha_2026-api1` boots healthy. `GET /ready` returns 200. `POST /fraud-score` returns 200 with `{"approved":true,"fraud_score":0.0000}` for both canonical legit and canonical fraud payloads — i.e. the API is currently a constant function (driven by C3 + C1 + the 1-record stub).

5. **Load test under spec conditions (k6)** — **Not performed.** Blocked on C8 (payload shape mismatch in `example-payloads.json`) and C4 (no real dataset).

6. **Resource hold under contention** — **Not performed.** Same blockers as 5.

---

## Open questions before any fix work begins

1. **Source of the real `references.json.gz` (3M vectors).** Without it, no end-to-end fix can be validated. (Audit blocker for steps 3, 5, 6.)
2. **Path forward on C6 (graph build).** Pure-Go HNSW library vs. write-it-in-tree vs. switch to IVF / VP-Tree.
3. **Quantization scheme.** Keep `uint8`-with-0-sentinel (3× memory savings, SIMD-friendly distance) or revert to `float32` with literal `-1.0` (matches spec wording, costs ~168 MB for the data array).

---

## File index (where findings live)

| File | Findings |
|---|---|
| `main.go:200–240` | C1 (vectorization), C2 (sentinel) |
| `main.go:112–171` | C7 (search), M1 (visited bitset) |
| `main.go:255` | M2 (response formatting) |
| `main.go:262–271, 275–301` | C5 (HnswM mismatch slice OOB at scale) |
| `main.go:316` | M3 (reuseport) |
| `main.go:185–258` | M4 (404 vs 405) |
| `tools/build_hnsw.go:96–98` | C3 (label drop) |
| `tools/build_hnsw.go:117–145` | C6 (graph build) |
| `tools/build_hnsw.go:14–17` | C5 (MaxNeighbors=8 hardcoded) |
| `resources/references.json.gz` | C4 (1-record stub) |
| `resources/example-payloads.json` | C8 (wrong schema) |
| `resources/normalization.json` | H2 (absent) |
| `resources/mcc_risk.json` | confirm against spec (currently 4 codes; spec lists 10) |
| `.env` | C5 (HNSW_M=16) |
| `haproxy.cfg:2` | H3 (nbthread) |
| `haproxy.cfg:21–22` | H4 (TCP-only backend check) |
| `docker-compose.yml:38, 56` | L1 (stale comment) |
| `Dockerfile:14` | L2 (build-time HNSW) |
| `cmd/healthcheck/main.go` | H4 |
| `k6/test.js` | blocked by C8 |
