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

/* Quantize a float to byte using Go's clampQuantize encoding:
 *   x < 0  → 0      (sentinel for missing / -1)
 *   x>1    → 255
 *   else   → round(x*254) + 1   (so 0.0 → 1, 1.0 → 255)
 * The 0-byte sentinel must NOT be produced by normalized values.
 * Uses banker-free rounding (round half away from zero) to match math.Round. */
uint8_t clamp_quantize(double x);

/* Per-MCC risk lookup (matches resources/mcc_risk.json values; default 0.5). */
double mcc_risk(int mcc);

/* Convert a decoded Payload into a 16-byte vector (bytes 14/15 zero pad).
 * Output byte layout is the SAME as Go v0.12's vectorize() — verified by
 * test_vectorize.c against canonical legit/fraud payloads. */
void vectorize_payload(const Payload *p, uint8_t out_vec[RECORD_STRIDE]);

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
