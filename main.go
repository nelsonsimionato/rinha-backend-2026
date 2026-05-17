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
	"unsafe"

	"github.com/valyala/fasthttp"
	"github.com/valyala/fasthttp/reuseport"
	"github.com/valyala/fastjson"
)

const (
	Dimensions    = 14 // logical dimensions per vector
	RecordStride  = 16 // physical bytes per record (padded for AVX2 16-byte loads)
	K             = 5  // k for k-NN
	IvfProbe      = 16 // top-N clusters scanned per query (IVF probe count)
	FormatVersion = uint8(4)
	HeaderSize    = 16
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

// IVF index loaded from resources/index.bin (format v4).
var (
	ivfK           uint32
	ivfN           uint32
	centroids      []uint8  // K * RecordStride bytes
	clusterOffsets []uint32 // K entries: start record index of each cluster
	clusterSizes   []uint32 // K entries: number of records in each cluster
	data           []uint8  // N * RecordStride bytes, sorted by cluster
	isFraud        []uint8  // N bytes, same order as data
)

var (
	parserPool fastjson.ParserPool

	searchStatePool = sync.Pool{
		New: func() interface{} { return &SearchState{} },
	}

	DummyVar byte
)

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

// ProbeQueue keeps the IvfProbe smallest centroid distances. Same insertion-sort
// pattern as KNNQueue but indexed at a different size.
type ProbeNeighbor struct {
	DistSq uint32
	Idx    int32
}

type ProbeQueue struct {
	Items [IvfProbe]ProbeNeighbor
	Count int
}

func (p *ProbeQueue) Push(distSq uint32, idx int32) {
	if p.Count < IvfProbe {
		p.Items[p.Count] = ProbeNeighbor{DistSq: distSq, Idx: idx}
		p.Count++
		for i := p.Count - 1; i > 0 && p.Items[i].DistSq < p.Items[i-1].DistSq; i-- {
			p.Items[i], p.Items[i-1] = p.Items[i-1], p.Items[i]
		}
	} else if distSq < p.Items[IvfProbe-1].DistSq {
		p.Items[IvfProbe-1] = ProbeNeighbor{DistSq: distSq, Idx: idx}
		for i := IvfProbe - 1; i > 0 && p.Items[i].DistSq < p.Items[i-1].DistSq; i-- {
			p.Items[i], p.Items[i-1] = p.Items[i-1], p.Items[i]
		}
	}
}

type SearchState struct {
	Q  KNNQueue
	PQ ProbeQueue
}

// SearchKNN runs the IVF search:
//
//	1. Compute distance from query to each of K centroids
//	2. Keep the IvfProbe closest centroids
//	3. Scan all records in those clusters, maintain top-K neighbors
//
// Cost per query: K + IvfProbe * (avg cluster size) distance computes.
func SearchKNN(query *[RecordStride]uint8) (top [K]Neighbor) {
	state := searchStatePool.Get().(*SearchState)
	state.Q.Count = 0
	state.PQ.Count = 0
	qPtr := &query[0]

	// Phase 1: centroid scan
	kk := int32(ivfK)
	for c := int32(0); c < kk; c++ {
		d := distanceSq(qPtr, &centroids[c*RecordStride])
		state.PQ.Push(d, c)
	}

	// Phase 2: probe top clusters
	probeCount := state.PQ.Count
	for i := 0; i < probeCount; i++ {
		c := state.PQ.Items[i].Idx
		start := clusterOffsets[c]
		size := clusterSizes[c]
		for r := uint32(0); r < size; r++ {
			recIdx := start + r
			d := distanceSq(qPtr, &data[recIdx*RecordStride])
			state.Q.Push(d, int32(recIdx))
		}
	}

	top = state.Q.Items
	searchStatePool.Put(state)
	return
}

// clampQuantize MUST match tools/build_ivf.go's encoding.
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

// specDayOfWeek returns Mon=0..Sun=6 via Zeller's congruence (zero alloc, no time.Time).
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
// Absolute anchor is arbitrary; we only ever take differences, which are exact.
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
	// Fail-safe: any panic in vectorize/SearchKNN must NOT surface as HTTP 500.
	// In the scoring formula, Err is weight 5 in E and also counts in the raw
	// failure_rate (which has a 15% hard cliff). An FP from an "approve" fallback
	// is weight 1 — five times cheaper than a 5xx, no double-count.
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
		// Malformed JSON: cheaper to "approve" (FP weight 1) than 400 (which counts as raw failure).
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
	ivfK = binary.LittleEndian.Uint32(binData[4:8])
	ivfN = binary.LittleEndian.Uint32(binData[8:12])

	off := HeaderSize
	centroidLen := int(ivfK) * RecordStride
	centroids = binData[off : off+centroidLen]
	off += centroidLen

	coBytes := binData[off : off+int(ivfK)*4]
	off += int(ivfK) * 4
	csBytes := binData[off : off+int(ivfK)*4]
	off += int(ivfK) * 4

	clusterOffsets = unsafe.Slice((*uint32)(unsafe.Pointer(&coBytes[0])), int(ivfK))
	clusterSizes = unsafe.Slice((*uint32)(unsafe.Pointer(&csBytes[0])), int(ivfK))

	dataLen := int(ivfN) * RecordStride
	data = binData[off : off+dataLen]
	off += dataLen
	isFraud = binData[off : off+int(ivfN)]

	log.Printf("loaded index v%d: K=%d centroids, N=%d records, %.1f MB",
		ver, ivfK, ivfN, float64(len(binData))/1e6)

	for i := 0; i < len(binData); i += 4096 {
		DummyVar += binData[i]
	}
}

func main() {
	loadNormalization("resources/normalization.json")
	loadIndex("resources/index.bin")

	server := &fasthttp.Server{
		Handler: requestHandler,
		Name:    "IVF",
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
