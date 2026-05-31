#ifndef VECTORIZE_H
#define VECTORIZE_H

#include "compat.h"

/* Decoded fraud-score payload — filled by json_parse() before vectorize_payload().
 * Strings are slices into the original HTTP body buffer (no copies). */
typedef struct {
	double      tx_amount;
	double      tx_installments;   /* parsed as number; vectorize divides by 12 */
	const char *tx_requested_at;
	int         tx_requested_at_len;

	double      cust_avg_amount;
	double      cust_tx_count_24h;
	/* known_merchants slices (each merchant_id is a string slice).
	 * The parser stores up to KNOWN_MERCHANTS_CAP entries. */
	const char *known_merchants[16];
	int         known_merchants_lens[16];
	int         known_merchants_count;

	const char *mer_id;
	int         mer_id_len;
	int         mer_mcc;
	int         mer_mcc_valid;
	double      mer_avg_amount;

	int         term_is_online;
	int         term_card_present;
	double      term_km_from_home;

	int         has_last_tx;
	const char *last_tx_timestamp;
	int         last_tx_timestamp_len;
	double      last_tx_km_from_current;
} Payload;

#define KNOWN_MERCHANTS_CAP 16

/* Quantize a float to int16 (scale I16_SCALE=5000), matching the builder
 * (tools/build_kdtree.go quantizeI16) bit-for-bit:
 *   x < 0  → I16_SENTINEL (-5000)   (null last_transaction / missing)
 *   x > 1  → 5000
 *   else   → round(x*5000)          (0.0 → 0, 1.0 → 5000)
 * Round half away from zero (lround) to match Go math.Round. */
int16_t quantize_i16(double x);

/* Per-MCC risk lookup (matches resources/mcc_risk.json values; default 0.5). */
double mcc_risk(int mcc);

/* Convert a decoded Payload into a 16-lane int16 vector (lanes 14/15 zero pad).
 * Output layout matches the index builder — verified by test_vectorize.c
 * against the canonical legit/fraud payloads. */
void vectorize_payload(const Payload *p, int16_t out_vec[RECORD_STRIDE]);

/* Normalization constants — overridable at startup by reading
 * resources/normalization.json. */
extern double MaxAmount;
extern double MaxInstallments;
extern double AmountVsAvgRatio;
extern double MaxMinutes;
extern double MaxKm;
extern double MaxTxCount24h;
extern double MaxMerchantAvgAmount;

#endif /* VECTORIZE_H */
