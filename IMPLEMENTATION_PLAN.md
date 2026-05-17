# IMPLEMENTATION_PLAN — Rinha 2026 Fraud Detection (Brute-Force KNN)

Follow-up to `AUDIT_FINDINGS.md`. Sequences the fixes for the audit's findings into phases with explicit gates between them. Keep the audit doc open as the reference for every finding code (C1, C3, …).

---

## Context

The audit established that the current implementation is a constant function (`approved:true, fraud_score:0` for every payload) because three independent bugs compound: vectorization writes wrong fields to wrong indices (C1), the builder silently drops every fraud label (C3), and the reference dataset is a 1-record stub (C4). The audit also flagged that the existing `tools/build_hnsw.go` does not actually build an HNSW graph (C6).

**This plan replaces the graph index entirely with brute-force exact k-NN.** Rationale:

- The spec (`docs/VECTOR_SEARCH.md:188`) explicitly allows brute force.
- The test labels its payloads using exact 5-NN with Euclidean distance — brute force matches this ground truth **by construction**, so detection score is `+3000` with zero recall risk. C6 / C7 / Phase-2 recall measurement all disappear.
- Memory drops from `~141 MB` (uint8 data + M=8 graph) to `~45 MB` (uint8 data + fraud flags). Comfortable inside the 150 MB/instance budget.
- The whole `tools/build_hnsw.go` simplifies into a JSON-decode-and-serialize tool.

The trade-off lives entirely in the latency column. Back-of-envelope on the Haswell-era test machine: a tight unrolled 14-byte distance loop runs ~5–10 ns per pair in pure Go, so 3M references is ~15–30 ms of CPU per query. Under the 0.40 vCPU/instance budget and k6's 50 VUs against 2 instances, realistic p99 is 100–500 ms — score band `+3300` to `+4000`. If p99 lands above 2000 ms the score floors at `-3000`, so the latency risk is real and Phase 3 is where the points are won.

Realistic score target: **+4000 to +5000**. Stretch (with SIMD assembly or subset partitioning): **+5500**.

---

## Decisions baked into this plan

| # | Decision | Default | Why |
|---|---|---|---|
| D1 | Source of real `references.json.gz` | User obtains from challenge organizers / repo `main` branch | External dependency |
| D2 | Search algorithm | **Brute-force exact k=5 Euclidean** over the full 3M reference set | Maxes detection score, simplest correct code, smallest memory footprint; escalation paths exist if latency floors |
| D3 | Vector storage precision | **Keep uint8 quantization** with the `0 = "missing or below-range"` sentinel | 3× memory savings; int32 distance is SIMD-friendly; existing `clampQuantize` already encodes this |
| D4 | Top-K data structure | 5-element sorted array (existing `KNNQueue`) | K=5 is small; O(K) insertion beats a heap for K this small |

**Latency escalation path (only used if Phase 3 measurement fails):**

1. **Subset partitioning by `last_transaction` presence.** Split the 3M references into two pools — "with-history" (idx 5/6 in [0,1]) and "no-history" (idx 5/6 == -1). A query routes to one pool based on its own `last_transaction == null` bit. Cuts scan size by ~2× with **zero recall loss**: the sentinel `-1` contributes squared-distance ≥ 4 (raw) or ≥ 65025 (quantized 0 vs 255) at idx 5/6 alone, so cross-pool neighbors are never in the top-5.
2. **AVX2 assembly for `euclideanDist`.** Hand-write `distance_amd64.s` to process distances in batches (16 vectors at a time using 32-byte registers, padding 14 → 16 dims). ~3–5× speedup over the unrolled Go loop. Pure-Go fallback path stays in `distance_generic.go` for non-amd64 builds.
3. **IVF (last resort, sacrifices recall).** Pre-cluster the 3M vectors into ~1700 centroids; probe top-4 clusters per query. ~99% recall, ~4–8× speedup. Only if 1+2 don't reach target.

---

## Phase 0 — Unblock

Goal: have the inputs needed to run Phase 1's tests.

| Finding | Action | File(s) |
|---|---|---|
| C4 | Obtain real `references.json.gz` (3M records, ~16 MB gzipped, spec format `{"vector":[...14 floats...],"label":"fraud"\|"legit"}`). If unavailable when Phase 1 starts, also produce a 10K-record synth set as a working stand-in. | `resources/references.json.gz` (replace); optional `tools/synth_refs.go` (build-tagged ignore) |
| C8 | Regenerate `resources/example-payloads.json` matching `docs/API.md` schema. Include: legit-no-history (use the worked example from `docs/DETECTION_RULES.md:13–21`), legit-with-history, fraud-no-history (the other worked example from `docs/DETECTION_RULES.md:110–119`), borderline. | `resources/example-payloads.json` (rewrite) |
| H2 | Add `resources/normalization.json` from the spec block in `docs/DATASET.md:58–67`. The runtime loads it in 1.2. | new `resources/normalization.json` |

**Gate to Phase 1:** `gzip -dc resources/references.json.gz \| jq '.[0]'` returns a record with `"label"` field; `example-payloads.json` parses cleanly with the spec-shape field names.

---

## Phase 1 — Correct the API end-to-end (small dataset)

Goal: the API computes the right vector, looks up against a correctly-built (small) flat index, and returns the right label on the canonical legit and fraud payloads from `docs/DETECTION_RULES.md`. Latency is not yet a concern.

### 1.1 — Delete the HNSW layer; introduce a flat index format

Replace `tools/build_hnsw.go` with `tools/build_index.go` (build-tagged ignore). The builder:

- Streams `resources/references.json.gz` via `gzip.Reader` + `json.Decoder` (avoids loading 284 MB uncompressed into memory).
- For each record: clamp-quantizes the 14 float dims to bytes via `clampQuantize`; appends a fraud-flag byte (1 for `"label":"fraud"`, 0 otherwise).
- Serializes a flat binary `resources/index.bin`:
  ```
  [0:4]   totalPoints (uint32 LE)
  [4:8]   dimensions  (uint32 LE)   // always 14, stored for sanity
  [8:9]   formatVersion (uint8)     // bump if layout changes
  [9:12]  padding
  [12:12+N*14]      data (uint8[N][14])
  [12+N*14:12+N*15] isFraud (uint8[N])
  ```
- Refuses to start if any decode error occurs (replace the silent `if err == nil` skip with `log.Fatalf`, per C3).

Runtime (`main.go`):

- Drop `HnswM`, `HnswEfConstruction`, `graph`, the whole `SearchHNSW` function, the `bitsetPool`, the `Candidates` field of `SearchState`.
- `SearchState` becomes just `Q KNNQueue` (the existing 5-element sorted array).
- New `SearchKNN(query)`:
  ```
  state := pool.Get(); state.Q.Count = 0
  n := int32(len(data) / Dimensions)
  for i := int32(0); i < n; i++ {
      state.Q.Push(euclideanDist(query, i), i, 5)
  }
  copy(top5[:], state.Q.Items[:5])
  pool.Put(state); return
  ```
- Loader reads `index.bin` (no graph section to slice), zero-copy via `unsafe.Slice` for the `data` byte view.

C5 (HnswM mismatch) disappears with the graph. Remove `HNSW_M=` and `HNSW_EF_CONSTRUCTION=` from `.env`.

### 1.2 — Rewrite vectorization to match spec (C1)

Replace `main.go:200–240` with code that writes the spec's 14 indices in order. Key points:

1. **Load `resources/normalization.json` at startup** into package-level constants — no map lookups on the hot path. Hardcoded defaults if the file is absent (H2).
2. **Parse `transaction.requested_at` byte-by-byte** — fixed ISO-8601 format `2026-03-11T18:45:53Z`. Hour = `(b[11]-'0')*10 + (b[12]-'0')`. Day-of-week via "days since 2000-01-01 (Sat=5)" arithmetic — no `time.Parse` allocations.
3. **Linear scan over `customer.known_merchants`** for `unknown_merchant` (idx 11). Spec payloads show 2–10 entries, no need for a map.
4. **`merchant.mcc` to int** (4 ASCII digits), then `MccRisk[code]` for idx 12.
5. **`last_transaction.timestamp`** → minutes between it and `transaction.requested_at`. Both are UTC ISO-8601 with `Z` suffix; subtract via the day-arithmetic + HH:MM:SS bytes, no `time.Parse`.
6. **`last_transaction == null`** → vec[5] = vec[6] = 0 directly (existing convention from D3); document with a one-line comment.

Add `main_vectorize_test.go` with two pin tests against the canonical legit (`[0.0041, 0.1667, 0.05, 0.7826, 0.3333, -1, -1, 0.0292, 0.15, 0, 1, 0, 0.15, 0.006]`) and fraud (`[0.9506, 0.8333, 1.0, 0.2174, 0.8333, -1, -1, 0.9523, 1.0, 0, 1, 1, 0.75, 0.0055]`) examples. Compare each expected float through `clampQuantize` → byte. **This test is the unmovable gate.**

### 1.3 — Fix builder label parsing (C3)

In the new `tools/build_index.go`:

```go
var row struct {
    Vector [Dimensions]float32 `json:"vector"`
    Label  string              `json:"label"`
}
if err := decoder.Decode(&row); err != nil { log.Fatalf(...) }
isFraudFlag := byte(0)
if row.Label == "fraud" { isFraudFlag = 1 }
```

Add `tools/inspect_bin.go` (build-tagged ignore) that prints header + fraud-label histogram. Commit it; useful for ongoing diagnostics.

### 1.4 — End-to-end correctness check

Hand-build a 50-record reference set (`tools/synth_refs.go`): 25 vectors clustered near the canonical legit vector with `label:legit`, 25 near the canonical fraud vector with `label:fraud`. Rebuild `index.bin`. Boot the container.

- `curl POST /fraud-score` with the canonical legit payload → expect `{"approved":true,"fraud_score":0.0000}`
- Same with the canonical fraud payload → expect `{"approved":false,"fraud_score":1.0000}`

Both currently return identical responses regardless of input — this test prevents that regression from coming back.

**Gate to Phase 2:** `main_vectorize_test.go` passes; both endpoint checks return spec-correct labels.

---

## Phase 2 — Index the real 3M dataset

Goal: the runtime brute-force scan operates correctly over the real 3M-vector reference set with predictable resource usage.

### 2.1 — Build the real index

Run `go run tools/build_index.go` outside Docker against the real `references.json.gz`. Expected output: `resources/index.bin` of size `12 + 3_000_000 * 15 = ~45 MB`. Inspect via `tools/inspect_bin.go`:

- `totalPoints == 3000000`
- Fraud-label histogram ratio matches whatever the spec / preview tests imply (likely 10–30% fraud).
- File size matches the arithmetic.

Then `COPY resources/index.bin` into the Docker image (`Dockerfile`) instead of running the builder inside Docker — keeps `docker build` to seconds, not minutes. The build step `RUN go run tools/build_index.go` is **removed from the Dockerfile**.

### 2.2 — Boot at scale + memory measurement

`docker compose up`. Watch:

- Startup logs: index loads, pre-warm runs (the existing `for i := 0; i < len(binData); i += 4096 { DummyVar += binData[i] }` pattern stays — it forces page faults so the first 50 queries don't pay them).
- `docker stats`: per-instance memory should sit around 80–110 MB (Go runtime ~50–70 MB + index ~45 MB). HAProxy ~30 MB. **Total ≤ 350 MB.**
- `curl :9999/ready` returns 200; `curl :9999/fraud-score -d @example-payloads.json[0]` returns a well-formed body.

**Gate to Phase 3:** memory under budget; smoke endpoint check passes against the real index.

---

## Phase 3 — Latency

Goal: under k6 (5000 iters, 50 VUs, port 9999 via HAProxy), p99 stays well below the 2000 ms `-3000` floor and ideally below 100 ms. Each sub-step measures first, then optimizes only if needed.

### 3.1 — Baseline measurement

Update `k6/test.js` thresholds to realistic targets:

```js
http_req_duration: [{ threshold: 'p(99)<500', abortOnFail: false }],
http_req_failed:   ['rate==0.00'],
```

Run `make test`. Record the baseline p99 from the brute-force unrolled-Go-loop implementation. Three scenarios from here:

- **p99 < 100 ms** → skip 3.2–3.5; jump to Phase 4 (you've earned `+4000–5000`).
- **100 ms ≤ p99 < 500 ms** → apply 3.2 + 3.3, remeasure.
- **p99 ≥ 500 ms** → apply 3.2 + 3.3 + 3.4 in order, remeasure after each.

### 3.2 — Free wins (independent of search loop)

| # | Finding | Action | File |
|---|---|---|---|
| 3.2.1 | H3 | `nbthread 2` → `nbthread 1` | `haproxy.cfg:2` |
| 3.2.2 | H4 | TCP backend check → HTTP `/ready` check (`option httpchk GET /ready` + `server … check`) | `haproxy.cfg` |
| 3.2.3 | M2 | Replace `fmt.Fprintf` with precomputed-string lookup. Six possible `(approved, fraud_score)` pairs: `(true, 0.0/0.2/0.4)` and `(false, 0.6/0.8/1.0)`. Indexed by `fraudCount`. `ctx.SetBodyString(responseTable[fraudCount])`. | `main.go:255` |
| 3.2.4 | M3 | `reuseport.Listen` → `net.Listen` | `main.go:316` |
| 3.2.5 | M4 | Return 405 for wrong methods | `main.go:185–258` |

### 3.3 — Subset partitioning by `last_transaction` (escalation step 1)

Split the index into two flat arrays at build time:

- `dataWithHistory`, `isFraudWithHistory` — records whose source had non-null `last_transaction`
- `dataNoHistory`, `isFraudNoHistory` — records whose source had `last_transaction: null`

Easy to detect during build: a record is "no-history" iff its raw float vector has -1.0 at idx 5 or 6 (`clampQuantize` will turn those into 0, but the builder still has the floats).

Extend the binary format with a section table:

```
[header...]
[pool A: data + isFraud]
[pool B: data + isFraud]
```

Runtime: read both pools, dispatch in `SearchKNN` based on `query[5] == 0 && query[6] == 0`. ~50% reduction in scan size with **zero recall change** — the -1 sentinel forces ≥ 65025 squared-distance contribution between pools, so cross-pool neighbors are never in the top-5.

### 3.4 — AVX2 distance (escalation step 2)

Add `distance_amd64.s` + `distance_amd64.go` exposing `func batchDist(query *[16]uint8, data unsafe.Pointer, n int, out []uint32)`. Pad the 14-D vectors to 16-D (zero-pad indices 14 and 15) at build time. The kernel:

- Loads `query` once into ymm0.
- For each batch of 16 reference vectors, load 256 bytes via `vmovdqu`.
- `vpsadbw` for unsigned absolute difference summed (gives L1 per 8-byte sub-vector; for squared Euclidean replace with `vpsubb` → sign-extend → `vpmullw` → `vpaddw`).
- Writes 16 distances to `out`.

Keep the pure-Go `euclideanDist` as the non-amd64 fallback path. Build tag: `//go:build amd64` vs `//go:build !amd64`.

Estimated speedup: 3–5× on the inner loop.

### 3.5 — Profile and tune (only if still slow)

`go tool pprof` against the API binary under k6 load. Expected hotspots after 3.2–3.4: memory bandwidth on the `data` scan (no further pure-CPU speedup possible without IVF). If memory bandwidth is the bottleneck, the only remaining option is IVF (escalation step 3) — implement only as last resort because it introduces recall risk.

**Gate to Phase 4:** k6 reports p99 < 500 ms and `http_req_failed == 0`. Memory + CPU stay under spec budget during the run.

---

## Phase 4 — Submission polish

| # | Finding | Action |
|---|---|---|
| 4.1 | M5 | Delete `api`, `main`, `rinha_2026` from repo root. Add `*.bin`, `/api`, `/main`, `/rinha_2026`, `/healthcheck` to `.gitignore` |
| 4.2 | L1 | Remove the "VP-Tree construction" comments in `docker-compose.yml:38, 56` |
| 4.3 | L3 | Decide: keep `.env` committed (current) or rename to `.env.example`. With brute force there are no tunables left in `.env`, so the file can probably go away entirely. |
| 4.4 | submission | Verify `info.json` matches `docs/SUBMISSION.md` (participants, social, source-code-repo, stack: `["go", "haproxy"]`, open_to_work) |
| 4.5 | submission | Verify image builds for `linux-amd64` explicitly (Apple Silicon dev → buildx `--platform linux/amd64`) |
| 4.6 | submission | All referenced images publicly pullable; participant image pushed to a public registry |
| 4.7 | submission | LICENSE file at root, MIT |

**Done criteria:** fresh clone → `docker compose up` → `curl :9999/ready` returns 200 → k6 5000 iters reports `http_req_failed == 0` and `p99 < 500ms` → `docker stats` shows total memory ≤ 350 MB and total CPU ≤ 1.0 sustained.

---

## File map (changes by phase)

| File | Phase | Change |
|---|---|---|
| `main.go` | 1.1 | Delete `SearchHNSW`, `bitsetPool`, `HnswM/HnswEfConstruction`, `graph`. Add `SearchKNN`. Loader reads flat `index.bin`. |
| `main.go` | 1.2 | Rewrite `requestHandler` vectorization to match spec |
| `main.go` | 3.2.3/4/5 | Response lookup table, `net.Listen`, 405 returns |
| `main_vectorize_test.go` | 1.2 | New — pins canonical examples |
| `tools/build_index.go` | 1.1, 1.3 | New (replaces `tools/build_hnsw.go`) |
| `tools/build_hnsw.go` | 1.1 | **Delete** |
| `tools/inspect_bin.go` | 1.3 | New (diagnostic) |
| `tools/synth_refs.go` | 0, 1.4 | New (if real dataset is delayed; also drives the small-set correctness check) |
| `distance_amd64.s` | 3.4 | New (conditional, only if needed) |
| `distance_amd64.go` | 3.4 | New (assembly wrapper) |
| `distance_generic.go` | 3.4 | Moves existing `euclideanDist` body here |
| `resources/references.json.gz` | 0 | Replace |
| `resources/example-payloads.json` | 0 | Rewrite |
| `resources/normalization.json` | 0 | New |
| `resources/index.bin` | 2.1 | Generated outside Docker, `COPY`'d in |
| `cmd/healthcheck/main.go` | — | Unchanged |
| `.env` | 1.1, 4.3 | Remove HNSW vars; possibly delete file |
| `Dockerfile` | 2.1 | Remove `RUN go run tools/build_hnsw.go`; `COPY resources/index.bin` instead |
| `Dockerfile` | 4.5 | `--platform linux/amd64` build verification |
| `docker-compose.yml` | 4.2 | Remove stale VP-Tree comments |
| `haproxy.cfg` | 3.2.1, 3.2.2 | `nbthread 1`, HTTP backend checks |
| `k6/test.js` | 3.1 | Update p99 threshold to 500 ms |
| `.gitignore` | 4.1 | Add build artifacts + `*.bin` |
| `LICENSE` | 4.7 | New, MIT |

---

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Real dataset's field name or vector order differs from `docs/DATASET.md` | Medium | Phase 1 blocked | Phase 0 verifies first record's shape before Phase 1 starts |
| Brute-force p99 lands above 500 ms baseline | High | -1000 to -2000 points | Phase 3 escalation: subset partition → AVX2 → (last resort) IVF |
| Brute-force p99 lands above 2000 ms even after 3.4 | Low-Medium | -3000 floor; submission unviable | Last-resort IVF in 3.5; if recall drop blows the 15% gate, that's a -3000 either way, so try IVF |
| Memory exceeds 150 MB/instance after Go runtime overhead | Low | OOM kills | Drop the per-state `Q.Items` pool (5×8 bytes is nothing); set `GOGC=50` (already in Dockerfile); if still tight, run a single-instance variant with 0.80 vCPU and 300 MB |
| Submission branch image not linux-amd64 (typical on Apple Silicon dev) | Medium | Test environment rejects | 4.5: explicit `--platform linux/amd64` build |
| HAProxy backend check too aggressive after cold start | Medium | Initial requests fail | 3.2.2 + extend `start_period` in compose healthcheck if needed |
| Date/time hand-parser bug (off-by-one weekday, leap-year edge cases) | Medium | Vectorization parity test fails | Pin test in 1.2 catches it before any real-data run; include 2024-02-29 and a Sunday in test cases |

---

## What's *not* in this plan

- **No HNSW.** The decision is documented in D2; if the latency floor is breached even after AVX2 + subset partitioning, IVF is the escalation, not HNSW.
- **No new abstractions or interfaces.** Direct rewrites only — the 14 dimensions are fixed by spec, the index is a flat byte array, the response has six possible values.
- **No tests beyond the vectorization pin test.** The competition is scored by k6, not unit tests; one anchor test for the most-broken layer is enough to prevent regression of the worst bug.
- **No performance optimization without a profile to point at it.** Phase 3 measures before tuning at every step.
