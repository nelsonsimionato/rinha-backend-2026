#include <math.h>
#include <string.h>
#include "vectorize.h"
#include "time_math.h"

/* Normalization constants — match docs/DATASET.md spec values. */
double MaxAmount            = 10000.0;
double MaxInstallments      = 12.0;
double AmountVsAvgRatio     = 10.0;
double MaxMinutes           = 1440.0;
double MaxKm                = 1000.0;
double MaxTxCount24h        = 20.0;
double MaxMerchantAvgAmount = 10000.0;

int16_t quantize_i16(double x)
{
	if (x < 0.0) return I16_SENTINEL;          /* null/missing -> -5000 */
	if (x > 1.0) return I16_SCALE;             /* clamp to 5000 */
	/* round-half-away-from-zero (lround) to match Go math.Round */
	return (int16_t)lround(x * (double)I16_SCALE);
}

double mcc_risk(int mcc)
{
	switch (mcc) {
		case 5411: return 0.15;
		case 5812: return 0.30;
		case 5912: return 0.20;
		case 5944: return 0.45;
		case 7801: return 0.80;
		case 7802: return 0.75;
		case 7995: return 0.85;
		case 4511: return 0.35;
		case 5311: return 0.25;
		case 5999: return 0.50;
		default:   return 0.50;
	}
}

void vectorize_payload(const Payload *p, int16_t vec[RECORD_STRIDE])
{
	/* zero-init (lanes 14/15 stay zero — AVX2 padding contract) */
	memset(vec, 0, RECORD_STRIDE * sizeof(int16_t));

	/* idx 0: amount */
	vec[0] = quantize_i16(p->tx_amount / MaxAmount);

	/* idx 1: installments */
	vec[1] = quantize_i16(p->tx_installments / MaxInstallments);

	/* idx 2: amount_vs_avg */
	if (p->cust_avg_amount > 0.0) {
		vec[2] = quantize_i16((p->tx_amount / p->cust_avg_amount) / AmountVsAvgRatio);
	} else {
		vec[2] = quantize_i16(1.0);
	}

	/* idx 3, 4: hour_of_day, day_of_week */
	int tx_y, tx_mo, tx_d, tx_h, tx_mi, tx_s;
	int tx_ok = parse_iso(p->tx_requested_at, p->tx_requested_at_len,
	                      &tx_y, &tx_mo, &tx_d, &tx_h, &tx_mi, &tx_s);
	if (tx_ok) {
		vec[3] = quantize_i16((double)tx_h / 23.0);
		vec[4] = quantize_i16((double)spec_day_of_week(tx_y, tx_mo, tx_d) / 6.0);
	}

	/* idx 5, 6: minutes_since_last_tx, km_from_last_tx (or -5000 sentinel) */
	if (!p->has_last_tx) {
		vec[5] = I16_SENTINEL;
		vec[6] = I16_SENTINEL;
	} else {
		int ly, lmo, ld, lh, lmi, ls;
		int l_ok = parse_iso(p->last_tx_timestamp, p->last_tx_timestamp_len,
		                     &ly, &lmo, &ld, &lh, &lmi, &ls);
		if (l_ok && tx_ok) {
			int64_t minutes = minutes_between(tx_y, tx_mo, tx_d, tx_h, tx_mi, tx_s,
			                                  ly, lmo, ld, lh, lmi, ls);
			if (minutes < 0) minutes = 0;
			vec[5] = quantize_i16((double)minutes / MaxMinutes);
		}
		vec[6] = quantize_i16(p->last_tx_km_from_current / MaxKm);
	}

	/* idx 7: km_from_home */
	vec[7] = quantize_i16(p->term_km_from_home / MaxKm);

	/* idx 8: tx_count_24h */
	vec[8] = quantize_i16(p->cust_tx_count_24h / MaxTxCount24h);

	/* idx 9: is_online */
	vec[9]  = quantize_i16(p->term_is_online    ? 1.0 : 0.0);
	/* idx 10: card_present */
	vec[10] = quantize_i16(p->term_card_present ? 1.0 : 0.0);

	/* idx 11: unknown_merchant (inverted: 1 if NOT in known_merchants) */
	int is_known = 0;
	for (int i = 0; i < p->known_merchants_count; i++) {
		if (p->known_merchants_lens[i] == p->mer_id_len &&
		    memcmp(p->known_merchants[i], p->mer_id, (size_t)p->mer_id_len) == 0) {
			is_known = 1;
			break;
		}
	}
	vec[11] = quantize_i16(is_known ? 0.0 : 1.0);

	/* idx 12: mcc_risk */
	if (p->mer_mcc_valid && p->mer_mcc >= 0 && p->mer_mcc < 10000) {
		vec[12] = quantize_i16(mcc_risk(p->mer_mcc));
	} else {
		vec[12] = quantize_i16(0.5);
	}

	/* idx 13: merchant_avg_amount */
	vec[13] = quantize_i16(p->mer_avg_amount / MaxMerchantAvgAmount);
}
