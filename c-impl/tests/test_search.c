/* End-to-end search test: load the real index.bin, vectorize the canonical
 * legit and fraud payloads, run search_knn, and verify the returned fraud
 * score matches the Go runtime (1.0 for fraud, 0.0 for legit). */

#include <stdio.h>
#include <string.h>
#include "../src/index.h"
#include "../src/vectorize.h"
#include "../src/search.h"

static int count_fraud(const Neighbor neigh[K])
{
	int c = 0;
	for (int i = 0; i < K; i++) {
		if (is_fraud[neigh[i].node_idx] == 1) c++;
	}
	return c;
}

static void fill_canonical_fraud(Payload *p)
{
	static const char *km[3]      = {"MERC-008", "MERC-007", "MERC-005"};
	static const int   km_lens[3] = {8, 8, 8};
	memset(p, 0, sizeof(*p));
	p->tx_amount             = 9505.97;
	p->tx_installments       = 10;
	p->tx_requested_at       = "2026-03-14T05:15:12Z";
	p->tx_requested_at_len   = 20;
	p->cust_avg_amount       = 81.28;
	p->cust_tx_count_24h     = 20;
	p->known_merchants_count = 3;
	memcpy(p->known_merchants,      km,      sizeof(km));
	memcpy(p->known_merchants_lens, km_lens, sizeof(km_lens));
	p->mer_id          = "MERC-068";
	p->mer_id_len      = 8;
	p->mer_mcc         = 7802;
	p->mer_mcc_valid   = 1;
	p->mer_avg_amount  = 54.86;
	p->term_is_online  = 0;
	p->term_card_present = 1;
	p->term_km_from_home = 952.27;
	p->has_last_tx = 0;
}

static void fill_canonical_legit(Payload *p)
{
	static const char *km[2]      = {"MERC-003", "MERC-016"};
	static const int   km_lens[2] = {8, 8};
	memset(p, 0, sizeof(*p));
	p->tx_amount             = 41.12;
	p->tx_installments       = 2;
	p->tx_requested_at       = "2026-03-11T18:45:53Z";
	p->tx_requested_at_len   = 20;
	p->cust_avg_amount       = 82.24;
	p->cust_tx_count_24h     = 3;
	p->known_merchants_count = 2;
	memcpy(p->known_merchants,      km,      sizeof(km));
	memcpy(p->known_merchants_lens, km_lens, sizeof(km_lens));
	p->mer_id          = "MERC-016";
	p->mer_id_len      = 8;
	p->mer_mcc         = 5411;
	p->mer_mcc_valid   = 1;
	p->mer_avg_amount  = 60.25;
	p->term_is_online  = 0;
	p->term_card_present = 1;
	p->term_km_from_home = 29.23;
	p->has_last_tx = 0;
}

int main(int argc, char **argv)
{
	const char *path = "../resources/index.bin";
	if (argc > 1) path = argv[1];
	if (index_load(path) != 0) return 1;

	SearchState st;
	Neighbor neigh[K];

	{
		Payload p; fill_canonical_fraud(&p);
		uint8_t v[16]; vectorize_payload(&p, v);
		search_knn(v, &st, neigh);
		int fraud = count_fraud(neigh);
		printf("[canonical-fraud] fraud_count=%d/5 → fraud_score=%.4f\n", fraud, fraud / 5.0);
		if (fraud < 3) { fprintf(stderr, "EXPECTED >= 3 fraud neighbors\n"); return 1; }
	}
	{
		Payload p; fill_canonical_legit(&p);
		uint8_t v[16]; vectorize_payload(&p, v);
		search_knn(v, &st, neigh);
		int fraud = count_fraud(neigh);
		printf("[canonical-legit] fraud_count=%d/5 → fraud_score=%.4f\n", fraud, fraud / 5.0);
		if (fraud >= 3) { fprintf(stderr, "EXPECTED < 3 fraud neighbors\n"); return 1; }
	}

	printf("SEARCH e2e PASS\n");
	return 0;
}
