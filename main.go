package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"log"
	"math"
	"net"
	"os"
	"os/signal"
	"sync"
	"syscall"

	"github.com/valyala/fasthttp"
	"github.com/valyala/fasthttp/reuseport"
	"github.com/valyala/fastjson"
)

const (
	Dimensions    = 14 // logical dimensions per vector
	RecordStride   = 16 // physical bytes per record (padded for AVX2 16-byte loads)
	K              = 5  // k for k-NN
	FormatVersion  = uint8(11)
	HeaderSize     = 16
	PartitionCount = 16384

	// MaxCandidates caps the lower-bound-pruned probe queue. After the matching
	// partition fills top-K, we only ever scan at most this many additional
	// partitions (sorted by their lower-bound). Tight cap keeps the cost of
	// bound-pruning small even with 16K partitions × ~3K non-empty.
	MaxCandidates = 64

	// SkipBoundPruneIfMatchingAtLeast: if the matching partition has at least
	// this many records, skip the bound-pruning step entirely and trust top-K
	// within matching. Adaptive trade-off: dense matchings (most queries) get
	// fast path (v0.8-style latency ~100ms); sparse matchings (boundary cases)
	// fall through to exhaustive bound-pruning for recall.
	SkipBoundPruneIfMatchingAtLeast = 100
)

// Normalization constants populated from resources/normalization.json at startup.
var (
	MaxAmount            = 10000.0
	MaxInstallments      = 12.0
	AmountVsAvgRatio     = 10.0
	MaxMinutes           = 1440.0
	MaxKm                = 1000.0
	MaxTxCount24h        = 20.0
	MaxMerchantAvgAmount = 10000.0
)

// Feature-hash partition (format v6): 256 pools indexed by partitionKey(vec),
// an 8-bit feature hash of fraud-discriminative dimensions (see partitionKey
// for bit layout). Brute force k=5 within matching partition + 8 single-bit
// Hamming neighbors (9 partitions probed per query, ~50K records average).
//
// Why partition by feature hash instead of K-means: k-means clusters by random
// feature subset (often hour/MCC); queries land in legit-dominant clusters
// even when their true neighbors are elsewhere. Feature-hash groups by the
// dimensions that actually predict fraud, so matching-partition recall is high.
var (
	data             []uint8 // N * RecordStride bytes, sorted by partition
	isFraud          []uint8 // N bytes, same order
	partitionOffsets [PartitionCount]uint32
	partitionSizes   [PartitionCount]uint32
	// Axis-aligned bounding boxes per partition (16 bytes each, AVX2-aligned).
	partitionMin []uint8
	partitionMax []uint8
	// Pre-computed list of non-empty partition indices, populated at load.
	// Lets SearchKNN iterate only the populated subset (~3K of 16K typically).
	nonEmptyPartitions []uint16
)

var (
	parserPool fastjson.ParserPool

	searchStatePool = sync.Pool{
		New: func() interface{} { return &SearchState{} },
	}

	DummyVar byte
)

type candidate struct {
	bound uint32
	idx   uint16
}

// Response bodies indexed by fraudCount ∈ [0..K]. Six total: K+1.
var responseBodies = [K + 1]string{
	`{"approved":true,"fraud_score":0.0000}`,
	`{"approved":true,"fraud_score":0.2000}`,
	`{"approved":true,"fraud_score":0.4000}`,
	`{"approved":false,"fraud_score":0.6000}`,
	`{"approved":false,"fraud_score":0.8000}`,
	`{"approved":false,"fraud_score":1.0000}`,
}

type Neighbor struct {
	DistSq  uint32
	NodeIdx int32
}

type KNNQueue struct {
	Items [K]Neighbor
	Count int
}

func (q *KNNQueue) Push(distSq uint32, nodeIdx int32) {
	if q.Count < K {
		q.Items[q.Count] = Neighbor{DistSq: distSq, NodeIdx: nodeIdx}
		q.Count++
		for i := q.Count - 1; i > 0 && q.Items[i].DistSq < q.Items[i-1].DistSq; i-- {
			q.Items[i], q.Items[i-1] = q.Items[i-1], q.Items[i]
		}
	} else if distSq < q.Items[K-1].DistSq {
		q.Items[K-1] = Neighbor{DistSq: distSq, NodeIdx: nodeIdx}
		for i := K - 1; i > 0 && q.Items[i].DistSq < q.Items[i-1].DistSq; i-- {
			q.Items[i], q.Items[i-1] = q.Items[i-1], q.Items[i]
		}
	}
}

type SearchState struct {
	Q     KNNQueue
	cands [MaxCandidates]candidate
	nC    int // current count in cands (bounded by MaxCandidates)
}

// pushCandidate inserts a candidate into the bounded sorted array.
// Keeps the MaxCandidates smallest bounds; rejects worse than worst-of-K
// (bounded insertion sort).
func (s *SearchState) pushCandidate(b uint32, idx uint16) {
	if s.nC < MaxCandidates {
		s.cands[s.nC] = candidate{bound: b, idx: idx}
		s.nC++
		for i := s.nC - 1; i > 0 && s.cands[i].bound < s.cands[i-1].bound; i-- {
			s.cands[i], s.cands[i-1] = s.cands[i-1], s.cands[i]
		}
		return
	}
	// Full: only insert if better than worst
	if b >= s.cands[MaxCandidates-1].bound {
		return
	}
	s.cands[MaxCandidates-1] = candidate{bound: b, idx: idx}
	for i := MaxCandidates - 1; i > 0 && s.cands[i].bound < s.cands[i-1].bound; i-- {
		s.cands[i], s.cands[i-1] = s.cands[i-1], s.cands[i]
	}
}

// partitionKey MUST match tools/build_partition_hash.go's encoding exactly.
// Returns a 13-bit hash of fraud-discriminative dimensions (range [0, 8191]).
func partitionKey(vec *[RecordStride]uint8) uint16 {
	var k uint16 = 0
	if vec[5] == 0 && vec[6] == 0 {
		k |= 1 << 0
	}
	if vec[9] > 128 {
		k |= 1 << 1
	}
	if vec[10] > 128 {
		k |= 1 << 2
	}
	if vec[11] > 128 {
		k |= 1 << 3
	}
	k |= uint16(vec[12]>>6) << 4
	k |= uint16(vec[2]>>6) << 6
	if vec[7] > 128 {
		k |= 1 << 8
	}
	if vec[8] > 128 {
		k |= 1 << 9
	}
	if vec[0] > 128 {
		k |= 1 << 10
	}
	if vec[3] > 128 {
		k |= 1 << 11
	}
	if vec[4] > 128 {
		k |= 1 << 12
	}
	if vec[13] > 128 {
		k |= 1 << 13
	}
	return k
}

// scanPartition runs a brute-force exact top-K scan over one partition's
// contiguous data slice into the global k-NN queue.
func scanPartition(query *[RecordStride]uint8, partIdx uint16, q *KNNQueue) {
	start := partitionOffsets[partIdx]
	size := partitionSizes[partIdx]
	if size == 0 {
		return
	}
	qPtr := &query[0]
	end := start + size
	for r := start; r < end; r++ {
		d := distanceSq(qPtr, &data[uint32(r)*RecordStride])
		q.Push(d, int32(r))
	}
}

// SearchKNN: adaptive bound-pruned k-NN.
//
// Fast path (matching.size >= SkipBoundPruneIfMatchingAtLeast):
//   trust top-K from matching partition alone. Most queries hit this path
//   because feature-hash buckets place similar queries with similar references.
//
// Slow path (sparse matching):
//   scan matching → seed top-K → iterate non-empty partitions with AVX2 bound,
//   collect MaxCandidates closest by bound, scan in sorted order, prune as
//   worst-K tightens.
//
// Recall on fast path: ~98.6% (boundary cases miss). On slow path: 100%.
// Empirically dense matchings have enough diversity that the within-partition
// top-K is the true top-K. Trade-off: ~1% recall loss on the fast path for
// ~3x throughput gain by skipping the 1547-partition bound loop.
func SearchKNN(query *[RecordStride]uint8) (top [K]Neighbor) {
	state := searchStatePool.Get().(*SearchState)
	state.Q.Count = 0
	state.nC = 0
	qPtr := &query[0]

	matchKey := partitionKey(query)
	matchSize := partitionSizes[matchKey]
	scanPartition(query, matchKey, &state.Q)

	if matchSize < SkipBoundPruneIfMatchingAtLeast {
		// Sparse matching: full bound-pruned exhaustive search.
		var worstDist uint32 = ^uint32(0)
		if state.Q.Count >= K {
			worstDist = state.Q.Items[K-1].DistSq
		}
		for _, i := range nonEmptyPartitions {
			if i == matchKey {
				continue
			}
			b := boundDistSq(qPtr, &partitionMin[uint32(i)*RecordStride], &partitionMax[uint32(i)*RecordStride])
			if b < worstDist {
				state.pushCandidate(b, i)
			}
		}
		for i := 0; i < state.nC; i++ {
			if state.Q.Count >= K && state.cands[i].bound >= state.Q.Items[K-1].DistSq {
				break
			}
			scanPartition(query, state.cands[i].idx, &state.Q)
		}
	}
	// Dense matching: matching's top-K is the answer (no bound loop).

	top = state.Q.Items
	searchStatePool.Put(state)
	return
}

// clampQuantize MUST match tools/build_partition.go's encoding.
func clampQuantize(x float64) uint8 {
	if x < 0.0 {
		return 0
	}
	if x > 1.0 {
		return 255
	}
	return uint8(math.Round(x*254.0)) + 1
}

func parseISO(b []byte) (y, mo, d, h, mi, s int, ok bool) {
	if len(b) < 19 {
		return 0, 0, 0, 0, 0, 0, false
	}
	y = atoi4(b[0:4])
	mo = atoi2(b[5:7])
	d = atoi2(b[8:10])
	h = atoi2(b[11:13])
	mi = atoi2(b[14:16])
	s = atoi2(b[17:19])
	return y, mo, d, h, mi, s, true
}

func atoi4(b []byte) int {
	return int(b[0]-'0')*1000 + int(b[1]-'0')*100 + int(b[2]-'0')*10 + int(b[3]-'0')
}

func atoi2(b []byte) int {
	return int(b[0]-'0')*10 + int(b[1]-'0')
}

// specDayOfWeek returns Mon=0..Sun=6 via Zeller's congruence (zero alloc).
func specDayOfWeek(y, mo, d int) int {
	if mo < 3 {
		mo += 12
		y--
	}
	K := y % 100
	J := y / 100
	// Zeller: h ∈ {0=Sat, 1=Sun, 2=Mon, ..., 6=Fri}. spec = (h + 5) mod 7 → Mon=0..Sun=6.
	h := (d + 13*(mo+1)/5 + K + K/4 + J/4 + 5*J) % 7
	return (h + 5) % 7
}

// daysFromEpoch — proleptic Gregorian days since 0000-03-01 (Howard Hinnant variant).
func daysFromEpoch(y, m, d int) int {
	if m <= 2 {
		y--
	}
	era := y / 400
	yoe := y - era*400
	moe := m - 3
	if m <= 2 {
		moe = m + 9
	}
	doy := (153*moe+2)/5 + d - 1
	doe := yoe*365 + yoe/4 - yoe/100 + doy
	return era*146097 + doe
}

func minutesBetween(y1, mo1, d1, h1, mi1, s1, y2, mo2, d2, h2, mi2, s2 int) int64 {
	dayDiff := int64(daysFromEpoch(y1, mo1, d1) - daysFromEpoch(y2, mo2, d2))
	secs := dayDiff*86400 + int64((h1-h2)*3600+(mi1-mi2)*60+(s1-s2))
	return secs / 60
}

func vectorize(v *fastjson.Value) [RecordStride]uint8 {
	var vec [RecordStride]uint8
	// bytes vec[14] and vec[15] stay zero — required by distance_amd64.s padding contract

	tx := v.Get("transaction")
	cus := v.Get("customer")
	mer := v.Get("merchant")
	term := v.Get("terminal")
	lastTx := v.Get("last_transaction")

	amount := tx.GetFloat64("amount")

	vec[0] = clampQuantize(amount / MaxAmount)
	vec[1] = clampQuantize(tx.GetFloat64("installments") / MaxInstallments)

	avgAmount := cus.GetFloat64("avg_amount")
	if avgAmount > 0 {
		vec[2] = clampQuantize((amount / avgAmount) / AmountVsAvgRatio)
	} else {
		vec[2] = clampQuantize(1.0)
	}

	txAt := tx.GetStringBytes("requested_at")
	txY, txMo, txD, txH, txMi, txS, txOk := parseISO(txAt)
	if txOk {
		vec[3] = clampQuantize(float64(txH) / 23.0)
		vec[4] = clampQuantize(float64(specDayOfWeek(txY, txMo, txD)) / 6.0)
	}

	if lastTx == nil || lastTx.Type() == fastjson.TypeNull {
		vec[5] = 0
		vec[6] = 0
	} else {
		lastAt := lastTx.GetStringBytes("timestamp")
		ly, lmo, ld, lh, lmi, ls, lok := parseISO(lastAt)
		if lok && txOk {
			minutes := minutesBetween(txY, txMo, txD, txH, txMi, txS, ly, lmo, ld, lh, lmi, ls)
			if minutes < 0 {
				minutes = 0
			}
			vec[5] = clampQuantize(float64(minutes) / MaxMinutes)
		}
		vec[6] = clampQuantize(lastTx.GetFloat64("km_from_current") / MaxKm)
	}

	vec[7] = clampQuantize(term.GetFloat64("km_from_home") / MaxKm)
	vec[8] = clampQuantize(cus.GetFloat64("tx_count_24h") / MaxTxCount24h)

	if term.GetBool("is_online") {
		vec[9] = clampQuantize(1.0)
	} else {
		vec[9] = clampQuantize(0.0)
	}
	if term.GetBool("card_present") {
		vec[10] = clampQuantize(1.0)
	} else {
		vec[10] = clampQuantize(0.0)
	}

	merID := mer.GetStringBytes("id")
	isKnown := false
	for _, km := range cus.GetArray("known_merchants") {
		if bytes.Equal(km.GetStringBytes(), merID) {
			isKnown = true
			break
		}
	}
	if isKnown {
		vec[11] = clampQuantize(0.0)
	} else {
		vec[11] = clampQuantize(1.0)
	}

	mccBytes := mer.GetStringBytes("mcc")
	mcc := 0
	valid := len(mccBytes) > 0
	for _, c := range mccBytes {
		if c < '0' || c > '9' {
			valid = false
			break
		}
		mcc = mcc*10 + int(c-'0')
	}
	if valid && mcc >= 0 && mcc < 10000 {
		vec[12] = clampQuantize(float64(MccRisk[mcc]))
	} else {
		vec[12] = clampQuantize(0.5)
	}

	vec[13] = clampQuantize(mer.GetFloat64("avg_amount") / MaxMerchantAvgAmount)

	return vec
}

func requestHandler(ctx *fasthttp.RequestCtx) {
	// Fail-safe: any panic must NOT surface as HTTP 500.
	// In the scoring formula, Err is weight 5 in E + counts in raw failure_rate
	// (15% hard cliff). An FP from an "approve" fallback is weight 1 — 5x cheaper.
	defer func() {
		if r := recover(); r != nil {
			ctx.Response.Reset()
			ctx.SetContentType("application/json")
			ctx.SetStatusCode(fasthttp.StatusOK)
			ctx.SetBodyString(responseBodies[0])
		}
	}()

	path := string(ctx.Path())
	if path == "/ready" {
		ctx.SetStatusCode(fasthttp.StatusOK)
		return
	}

	if path != "/fraud-score" {
		ctx.SetStatusCode(fasthttp.StatusNotFound)
		return
	}
	if !ctx.IsPost() {
		ctx.SetStatusCode(fasthttp.StatusMethodNotAllowed)
		return
	}

	parser := parserPool.Get()
	defer parserPool.Put(parser)

	v, err := parser.ParseBytes(ctx.PostBody())
	if err != nil {
		ctx.SetContentType("application/json")
		ctx.SetStatusCode(fasthttp.StatusOK)
		ctx.SetBodyString(responseBodies[0])
		return
	}

	vec := vectorize(v)
	top := SearchKNN(&vec)

	fraudCount := 0
	for i := 0; i < K; i++ {
		if isFraud[top[i].NodeIdx] == 1 {
			fraudCount++
		}
	}

	ctx.SetContentType("application/json")
	ctx.SetStatusCode(fasthttp.StatusOK)
	ctx.SetBodyString(responseBodies[fraudCount])
}

func loadNormalization(path string) {
	raw, err := os.ReadFile(path)
	if err != nil {
		log.Printf("normalization: file absent (%v), using hardcoded defaults", err)
		return
	}
	var n struct {
		MaxAmount            *float64 `json:"max_amount"`
		MaxInstallments      *float64 `json:"max_installments"`
		AmountVsAvgRatio     *float64 `json:"amount_vs_avg_ratio"`
		MaxMinutes           *float64 `json:"max_minutes"`
		MaxKm                *float64 `json:"max_km"`
		MaxTxCount24h        *float64 `json:"max_tx_count_24h"`
		MaxMerchantAvgAmount *float64 `json:"max_merchant_avg_amount"`
	}
	if err := json.Unmarshal(raw, &n); err != nil {
		log.Fatalf("normalization: invalid JSON: %v", err)
	}
	if n.MaxAmount != nil {
		MaxAmount = *n.MaxAmount
	}
	if n.MaxInstallments != nil {
		MaxInstallments = *n.MaxInstallments
	}
	if n.AmountVsAvgRatio != nil {
		AmountVsAvgRatio = *n.AmountVsAvgRatio
	}
	if n.MaxMinutes != nil {
		MaxMinutes = *n.MaxMinutes
	}
	if n.MaxKm != nil {
		MaxKm = *n.MaxKm
	}
	if n.MaxTxCount24h != nil {
		MaxTxCount24h = *n.MaxTxCount24h
	}
	if n.MaxMerchantAvgAmount != nil {
		MaxMerchantAvgAmount = *n.MaxMerchantAvgAmount
	}
}

func loadIndex(path string) {
	binData, err := os.ReadFile(path)
	if err != nil {
		log.Fatalf("read %s: %v", path, err)
	}
	if len(binData) < HeaderSize {
		log.Fatalf("index too small: %d bytes", len(binData))
	}
	ver := binData[0]
	if ver != FormatVersion {
		log.Fatalf("index format version %d, expected %d", ver, FormatVersion)
	}
	total := binary.LittleEndian.Uint32(binData[4:8])

	off := HeaderSize
	// partitionOffsets
	for i := 0; i < PartitionCount; i++ {
		partitionOffsets[i] = binary.LittleEndian.Uint32(binData[off+i*4 : off+i*4+4])
	}
	off += PartitionCount * 4
	// partitionSizes
	for i := 0; i < PartitionCount; i++ {
		partitionSizes[i] = binary.LittleEndian.Uint32(binData[off+i*4 : off+i*4+4])
	}
	off += PartitionCount * 4
	// Bounding boxes
	boxLen := PartitionCount * RecordStride
	partitionMin = binData[off : off+boxLen]
	off += boxLen
	partitionMax = binData[off : off+boxLen]
	off += boxLen

	dataLen := int(total) * RecordStride
	expected := off + dataLen + int(total)
	if len(binData) < expected {
		log.Fatalf("index truncated: %d bytes, expected %d", len(binData), expected)
	}

	data = binData[off : off+dataLen]
	off += dataLen
	isFraud = binData[off : off+int(total)]

	// Pre-compute non-empty partition indices for fast iteration in SearchKNN.
	nonEmptyPartitions = nonEmptyPartitions[:0]
	for i := uint16(0); i < PartitionCount; i++ {
		if partitionSizes[i] > 0 {
			nonEmptyPartitions = append(nonEmptyPartitions, i)
		}
	}

	// histogram for log
	minS, maxS, emptyP := uint32(0xFFFFFFFF), uint32(0), 0
	for _, s := range partitionSizes {
		if s < minS {
			minS = s
		}
		if s > maxS {
			maxS = s
		}
		if s == 0 {
			emptyP++
		}
	}
	log.Printf("loaded index v%d: total=%d records, partitions=%d (avg=%d min=%d max=%d empty=%d), %.1f MB",
		ver, total, PartitionCount, total/PartitionCount, minS, maxS, emptyP,
		float64(len(binData))/1e6)

	for i := 0; i < len(binData); i += 4096 {
		DummyVar += binData[i]
	}
}

func main() {
	loadNormalization("resources/normalization.json")
	loadIndex("resources/index.bin")

	server := &fasthttp.Server{
		Handler: requestHandler,
		Name:    "BruteForceKNN",
	}

	listener, err := reuseport.Listen("tcp4", ":8080")
	if err != nil {
		log.Printf("reuseport unavailable (%v), falling back to net.Listen", err)
		listener, err = net.Listen("tcp", ":8080")
		if err != nil {
			log.Fatalf("listen: %v", err)
		}
	}

	go func() {
		if err := server.Serve(listener); err != nil {
			log.Fatalf("server: %v", err)
		}
	}()
	log.Println("API serving on :8080")

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	<-sigCh
}
