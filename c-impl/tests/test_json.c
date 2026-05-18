/* Verifies json_parse() + vectorize_payload() + search_knn() produce the
 * correct fraud_score for canonical legit and fraud payloads from
 * docs/DETECTION_RULES.md. End-to-end check across all stages. */

#include <stdio.h>
#include <string.h>
#include "../src/json.h"
#include "../src/index.h"
#include "../src/vectorize.h"
#include "../src/search.h"

static const char *FRAUD_JSON =
    "{"
    "\"id\":\"tx-3330991687\","
    "\"transaction\":{\"amount\":9505.97,\"installments\":10,"
                     "\"requested_at\":\"2026-03-14T05:15:12Z\"},"
    "\"customer\":{\"avg_amount\":81.28,\"tx_count_24h\":20,"
                  "\"known_merchants\":[\"MERC-008\",\"MERC-007\",\"MERC-005\"]},"
    "\"merchant\":{\"id\":\"MERC-068\",\"mcc\":\"7802\",\"avg_amount\":54.86},"
    "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":952.27},"
    "\"last_transaction\":null"
    "}";

static const char *LEGIT_JSON =
    "{"
    "\"id\":\"tx-1329056812\","
    "\"transaction\":{\"amount\":41.12,\"installments\":2,"
                     "\"requested_at\":\"2026-03-11T18:45:53Z\"},"
    "\"customer\":{\"avg_amount\":82.24,\"tx_count_24h\":3,"
                  "\"known_merchants\":[\"MERC-003\",\"MERC-016\"]},"
    "\"merchant\":{\"id\":\"MERC-016\",\"mcc\":\"5411\",\"avg_amount\":60.25},"
    "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":29.23},"
    "\"last_transaction\":null"
    "}";

static const char *LEGIT_HISTORY_JSON =
    "{"
    "\"id\":\"tx-5512345678\","
    "\"transaction\":{\"amount\":87.50,\"installments\":1,"
                     "\"requested_at\":\"2026-03-15T14:23:10Z\"},"
    "\"customer\":{\"avg_amount\":95.30,\"tx_count_24h\":5,"
                  "\"known_merchants\":[\"MERC-005\",\"MERC-012\",\"MERC-020\"]},"
    "\"merchant\":{\"id\":\"MERC-005\",\"mcc\":\"5411\",\"avg_amount\":102.45},"
    "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":3.2},"
    "\"last_transaction\":{\"timestamp\":\"2026-03-15T11:45:00Z\","
                          "\"km_from_current\":0.8}"
    "}";

static int count_fraud(const Neighbor n[K])
{
	int c = 0;
	for (int i = 0; i < K; i++) if (is_fraud[n[i].node_idx] == 1) c++;
	return c;
}

static int run_case(const char *name, const char *json, int min_fraud, int max_fraud)
{
	Payload p;
	if (json_parse(json, (int)strlen(json), &p) < 0) {
		fprintf(stderr, "[%s] json_parse FAILED\n", name);
		return 1;
	}
	uint8_t v[16];
	vectorize_payload(&p, v);

	SearchState st;
	Neighbor neigh[K];
	search_knn(v, &st, neigh);
	int c = count_fraud(neigh);
	printf("[%s] fraud_count=%d/5 (expect %d..%d)\n", name, c, min_fraud, max_fraud);
	if (c < min_fraud || c > max_fraud) {
		fprintf(stderr, "[%s] OUT OF RANGE\n", name);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = "../resources/index.bin";
	if (argc > 1) path = argv[1];
	if (index_load(path) != 0) return 1;

	int rc = 0;
	rc |= run_case("fraud",         FRAUD_JSON,         3, 5);
	rc |= run_case("legit",         LEGIT_JSON,         0, 2);
	rc |= run_case("legit-history", LEGIT_HISTORY_JSON, 0, 2);

	/* Stress: malformed JSON should NOT crash; json_parse returns -1 cleanly. */
	const char *bad = "{\"transaction\": broken";
	Payload p;
	int parse_rc = json_parse(bad, (int)strlen(bad), &p);
	if (parse_rc != -1) {
		fprintf(stderr, "expected malformed JSON to return -1, got %d\n", parse_rc);
		rc = 1;
	} else {
		printf("[malformed] parser returned -1 cleanly\n");
	}

	if (rc == 0) printf("ALL JSON TESTS PASS\n");
	return rc;
}
