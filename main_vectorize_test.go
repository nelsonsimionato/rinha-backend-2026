package main

import (
	"testing"

	"github.com/valyala/fastjson"
)

// TestVectorizeCanonicalLegit pins the canonical legit example from
// docs/DETECTION_RULES.md (tx-1329056812) byte-by-byte. Expected bytes are
// derived by applying the spec's formula directly on the raw payload values
// and then running them through clampQuantize — so this test verifies both
// that the right field lands at the right index and that the right formula
// is applied at each index.
func TestVectorizeCanonicalLegit(t *testing.T) {
	payload := `{
      "id": "tx-1329056812",
      "transaction":      { "amount": 41.12, "installments": 2, "requested_at": "2026-03-11T18:45:53Z" },
      "customer":         { "avg_amount": 82.24, "tx_count_24h": 3, "known_merchants": ["MERC-003", "MERC-016"] },
      "merchant":         { "id": "MERC-016", "mcc": "5411", "avg_amount": 60.25 },
      "terminal":         { "is_online": false, "card_present": true, "km_from_home": 29.23 },
      "last_transaction": null
    }`

	expected := [RecordStride]uint8{
		clampQuantize(41.12 / 10000),               // 0: amount
		clampQuantize(2.0 / 12),                    // 1: installments
		clampQuantize((41.12 / 82.24) / 10),        // 2: amount_vs_avg
		clampQuantize(18.0 / 23),                   // 3: hour_of_day (UTC 18:45:53)
		clampQuantize(2.0 / 6),                     // 4: day_of_week (Wed=2)
		0,                                          // 5: -1 sentinel (last_tx null)
		0,                                          // 6: -1 sentinel
		clampQuantize(29.23 / 1000),                // 7: km_from_home
		clampQuantize(3.0 / 20),                    // 8: tx_count_24h
		clampQuantize(0.0),                         // 9: is_online=false
		clampQuantize(1.0),                         // 10: card_present=true
		clampQuantize(0.0),                         // 11: MERC-016 IS known → 0
		clampQuantize(float64(MccRisk[5411])),      // 12: mcc_risk (spec: 0.15)
		clampQuantize(60.25 / 10000),               // 13: merchant_avg_amount
	}

	v, err := fastjson.Parse(payload)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	actual := vectorize(v)

	if actual != expected {
		t.Fatalf("mismatch:\n  expected: %v\n  actual:   %v\n  diffs at indices: %v",
			expected, actual, diffIndices(expected, actual))
	}
}

// TestVectorizeCanonicalFraud pins the canonical fraud example from
// docs/DETECTION_RULES.md (tx-3330991687).
func TestVectorizeCanonicalFraud(t *testing.T) {
	payload := `{
      "id": "tx-3330991687",
      "transaction":      { "amount": 9505.97, "installments": 10, "requested_at": "2026-03-14T05:15:12Z" },
      "customer":         { "avg_amount": 81.28, "tx_count_24h": 20, "known_merchants": ["MERC-008", "MERC-007", "MERC-005"] },
      "merchant":         { "id": "MERC-068", "mcc": "7802", "avg_amount": 54.86 },
      "terminal":         { "is_online": false, "card_present": true, "km_from_home": 952.27 },
      "last_transaction": null
    }`

	expected := [RecordStride]uint8{
		clampQuantize(9505.97 / 10000),             // 0: amount
		clampQuantize(10.0 / 12),                   // 1: installments
		clampQuantize((9505.97 / 81.28) / 10),      // 2: amount_vs_avg (clamps to 1.0)
		clampQuantize(5.0 / 23),                    // 3: hour_of_day (UTC 05:15:12)
		clampQuantize(5.0 / 6),                     // 4: day_of_week (Sat=5)
		0,                                          // 5: -1 sentinel
		0,                                          // 6: -1 sentinel
		clampQuantize(952.27 / 1000),               // 7: km_from_home
		clampQuantize(20.0 / 20),                   // 8: tx_count_24h (clamps to 1.0)
		clampQuantize(0.0),                         // 9: is_online=false
		clampQuantize(1.0),                         // 10: card_present=true
		clampQuantize(1.0),                         // 11: MERC-068 NOT known → 1
		clampQuantize(float64(MccRisk[7802])),      // 12: mcc_risk (spec: 0.75)
		clampQuantize(54.86 / 10000),               // 13: merchant_avg_amount
	}

	v, err := fastjson.Parse(payload)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	actual := vectorize(v)

	if actual != expected {
		t.Fatalf("mismatch:\n  expected: %v\n  actual:   %v\n  diffs at indices: %v",
			expected, actual, diffIndices(expected, actual))
	}
}

// TestVectorizeLegitWithHistory exercises the non-null last_transaction path.
// The legit-with-history payload from resources/example-payloads.json:
//   - tx at 2026-03-15T14:23:10Z, last_tx at 2026-03-15T11:45:00Z (~158 min ago)
//   - km_from_current = 0.8
func TestVectorizeLegitWithHistory(t *testing.T) {
	payload := `{
      "id": "tx-5512345678",
      "transaction":      { "amount": 87.50, "installments": 1, "requested_at": "2026-03-15T14:23:10Z" },
      "customer":         { "avg_amount": 95.30, "tx_count_24h": 5, "known_merchants": ["MERC-005", "MERC-012", "MERC-020"] },
      "merchant":         { "id": "MERC-005", "mcc": "5411", "avg_amount": 102.45 },
      "terminal":         { "is_online": false, "card_present": true, "km_from_home": 3.2 },
      "last_transaction": { "timestamp": "2026-03-15T11:45:00Z", "km_from_current": 0.8 }
    }`

	// 14:23:10 - 11:45:00 = 2h 38min 10s = 158 minutes (integer floor).
	expectedMinutes := int64(158)

	expected := [RecordStride]uint8{
		clampQuantize(87.50 / 10000),
		clampQuantize(1.0 / 12),
		clampQuantize((87.50 / 95.30) / 10),
		clampQuantize(14.0 / 23),
		clampQuantize(6.0 / 6), // 2026-03-15 is Sunday → spec idx 6
		clampQuantize(float64(expectedMinutes) / 1440),
		clampQuantize(0.8 / 1000),
		clampQuantize(3.2 / 1000),
		clampQuantize(5.0 / 20),
		clampQuantize(0.0),
		clampQuantize(1.0),
		clampQuantize(0.0), // MERC-005 is in known
		clampQuantize(float64(MccRisk[5411])),
		clampQuantize(102.45 / 10000),
	}

	v, err := fastjson.Parse(payload)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	actual := vectorize(v)

	if actual != expected {
		t.Fatalf("mismatch:\n  expected: %v\n  actual:   %v\n  diffs at indices: %v",
			expected, actual, diffIndices(expected, actual))
	}
}

func diffIndices(a, b [RecordStride]uint8) []int {
	var diffs []int
	for i := 0; i < RecordStride; i++ {
		if a[i] != b[i] {
			diffs = append(diffs, i)
		}
	}
	return diffs
}
