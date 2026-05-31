/* Validates vectorize_payload() against the canonical examples from
 * docs/DETECTION_RULES.md, now in i16 (scale 5000). Null last_transaction
 * yields the I16_SENTINEL (-5000) at lanes 5,6; lanes 14,15 are zero pad. */

#include <stdio.h>
#include <string.h>
#include "../src/vectorize.h"

static int check_vec(const char *name, const int16_t got[16], const int16_t want[16])
{
	int mismatches = 0;
	for (int i = 0; i < 16; i++) {
		if (got[i] != want[i]) {
			if (mismatches++ < 4) {
				fprintf(stderr, "[%s] vec[%d] got=%d want=%d\n", name, i, got[i], want[i]);
			}
		}
	}
	if (mismatches > 0) {
		fprintf(stderr, "[%s] %d mismatch(es)\n", name, mismatches);
		return 1;
	}
	printf("[%s] PASS\n", name);
	return 0;
}

static int test_canonical_legit(void)
{
	/* tx-1329056812:
	 * transaction: amount=41.12, installments=2, requested_at=2026-03-11T18:45:53Z (Wed=2)
	 * customer:    avg_amount=82.24, tx_count_24h=3, known_merchants=[MERC-003, MERC-016]
	 * merchant:    id=MERC-016 (known), mcc=5411 (risk 0.15), avg_amount=60.25
	 * terminal:    is_online=false, card_present=true, km_from_home=29.23
	 * last_transaction: null
	 */
	static const char *km[2]    = {"MERC-003", "MERC-016"};
	static const int   km_lens[2] = {8, 8};
	Payload p = {
		.tx_amount             = 41.12,
		.tx_installments       = 2,
		.tx_requested_at       = "2026-03-11T18:45:53Z",
		.tx_requested_at_len   = 20,
		.cust_avg_amount       = 82.24,
		.cust_tx_count_24h     = 3,
		.known_merchants_count = 2,
		.mer_id                = "MERC-016",
		.mer_id_len            = 8,
		.mer_mcc               = 5411,
		.mer_mcc_valid         = 1,
		.mer_avg_amount        = 60.25,
		.term_is_online        = 0,
		.term_card_present     = 1,
		.term_km_from_home     = 29.23,
		.has_last_tx           = 0,
	};
	memcpy(p.known_merchants,      km,      sizeof(km));
	memcpy(p.known_merchants_lens, km_lens, sizeof(km_lens));

	int16_t got[16];
	vectorize_payload(&p, got);

	int16_t want[16] = {
		quantize_i16(41.12 / 10000),                /* 0 */
		quantize_i16(2.0 / 12),                     /* 1 */
		quantize_i16((41.12 / 82.24) / 10),         /* 2 */
		quantize_i16(18.0 / 23),                    /* 3 */
		quantize_i16(2.0 / 6),                      /* 4: Wed = 2 */
		I16_SENTINEL,                               /* 5: sentinel */
		I16_SENTINEL,                               /* 6: sentinel */
		quantize_i16(29.23 / 1000),                 /* 7 */
		quantize_i16(3.0 / 20),                      /* 8 */
		quantize_i16(0.0),                          /* 9: is_online=false */
		quantize_i16(1.0),                          /* 10: card_present=true */
		quantize_i16(0.0),                          /* 11: MERC-016 known */
		quantize_i16(mcc_risk(5411)),               /* 12 */
		quantize_i16(60.25 / 10000),                /* 13 */
		0, 0,                                        /* 14, 15: padding */
	};

	return check_vec("canonical-legit", got, want);
}

static int test_canonical_fraud(void)
{
	/* tx-3330991687:
	 * transaction: amount=9505.97, installments=10, requested_at=2026-03-14T05:15:12Z (Sat=5)
	 * customer:    avg_amount=81.28, tx_count_24h=20, known=[MERC-008, MERC-007, MERC-005]
	 * merchant:    id=MERC-068 (UNKNOWN), mcc=7802 (risk 0.75), avg_amount=54.86
	 * terminal:    is_online=false, card_present=true, km_from_home=952.27
	 * last_transaction: null
	 */
	static const char *km[3]      = {"MERC-008", "MERC-007", "MERC-005"};
	static const int   km_lens[3] = {8, 8, 8};
	Payload p = {
		.tx_amount             = 9505.97,
		.tx_installments       = 10,
		.tx_requested_at       = "2026-03-14T05:15:12Z",
		.tx_requested_at_len   = 20,
		.cust_avg_amount       = 81.28,
		.cust_tx_count_24h     = 20,
		.known_merchants_count = 3,
		.mer_id                = "MERC-068",
		.mer_id_len            = 8,
		.mer_mcc               = 7802,
		.mer_mcc_valid         = 1,
		.mer_avg_amount        = 54.86,
		.term_is_online        = 0,
		.term_card_present     = 1,
		.term_km_from_home     = 952.27,
		.has_last_tx           = 0,
	};
	memcpy(p.known_merchants,      km,      sizeof(km));
	memcpy(p.known_merchants_lens, km_lens, sizeof(km_lens));

	int16_t got[16];
	vectorize_payload(&p, got);

	int16_t want[16] = {
		quantize_i16(9505.97 / 10000),
		quantize_i16(10.0 / 12),
		quantize_i16((9505.97 / 81.28) / 10),       /* clamps to 1.0 */
		quantize_i16(5.0 / 23),                     /* hour 05 */
		quantize_i16(5.0 / 6),                      /* Sat = 5 */
		I16_SENTINEL, I16_SENTINEL,
		quantize_i16(952.27 / 1000),
		quantize_i16(20.0 / 20),                    /* clamps to 1.0 */
		quantize_i16(0.0),
		quantize_i16(1.0),
		quantize_i16(1.0),                          /* unknown merchant */
		quantize_i16(mcc_risk(7802)),
		quantize_i16(54.86 / 10000),
		0, 0,
	};

	return check_vec("canonical-fraud", got, want);
}

static int test_legit_with_history(void)
{
	/* tx-5512345678: 2026-03-15T14:23:10Z (Sun=6), last 2026-03-15T11:45:00Z, km=0.8
	 * 158 minutes between (14:23:10 - 11:45:00 = 2h 38min 10s floor = 158). */
	static const char *km[3]      = {"MERC-005", "MERC-012", "MERC-020"};
	static const int   km_lens[3] = {8, 8, 8};
	Payload p = {
		.tx_amount               = 87.50,
		.tx_installments         = 1,
		.tx_requested_at         = "2026-03-15T14:23:10Z",
		.tx_requested_at_len     = 20,
		.cust_avg_amount         = 95.30,
		.cust_tx_count_24h       = 5,
		.known_merchants_count   = 3,
		.mer_id                  = "MERC-005",
		.mer_id_len              = 8,
		.mer_mcc                 = 5411,
		.mer_mcc_valid           = 1,
		.mer_avg_amount          = 102.45,
		.term_is_online          = 0,
		.term_card_present       = 1,
		.term_km_from_home       = 3.2,
		.has_last_tx             = 1,
		.last_tx_timestamp       = "2026-03-15T11:45:00Z",
		.last_tx_timestamp_len   = 20,
		.last_tx_km_from_current = 0.8,
	};
	memcpy(p.known_merchants,      km,      sizeof(km));
	memcpy(p.known_merchants_lens, km_lens, sizeof(km_lens));

	int16_t got[16];
	vectorize_payload(&p, got);

	int16_t want[16] = {
		quantize_i16(87.50 / 10000),
		quantize_i16(1.0 / 12),
		quantize_i16((87.50 / 95.30) / 10),
		quantize_i16(14.0 / 23),
		quantize_i16(6.0 / 6),                      /* Sun = 6 */
		quantize_i16(158.0 / 1440),                 /* minutes between */
		quantize_i16(0.8 / 1000),
		quantize_i16(3.2 / 1000),
		quantize_i16(5.0 / 20),
		quantize_i16(0.0),
		quantize_i16(1.0),
		quantize_i16(0.0),                          /* MERC-005 is known */
		quantize_i16(mcc_risk(5411)),
		quantize_i16(102.45 / 10000),
		0, 0,
	};

	return check_vec("legit-with-history", got, want);
}

int main(void)
{
	int rc = 0;
	rc |= test_canonical_legit();
	rc |= test_canonical_fraud();
	rc |= test_legit_with_history();
	if (rc == 0) printf("ALL VECTORIZE TESTS PASS\n");
	return rc;
}
