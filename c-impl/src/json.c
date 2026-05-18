/* Hand-written JSON parser for the fraud-score schema.
 *
 * Design notes:
 *  - No allocations; strings are slices into the source buffer.
 *  - No escape handling: production payloads contain only [A-Za-z0-9:.-]
 *    inside strings, so this is safe. If escapes appear, the slice is wrong
 *    but the higher layers see a mismatch (FP fallback, never a crash).
 *  - Whitespace between tokens is skipped via skip_ws().
 *  - Unknown keys at any level are skipped via skip_value().
 *  - Returns -1 at the FIRST malformation; caller emits safe fallback.
 */

#include <stdlib.h>
#include <string.h>
#include "json.h"

typedef struct {
	const char *p;
	const char *end;
} Cur;

static inline void skip_ws(Cur *c)
{
	while (c->p < c->end) {
		char ch = *c->p;
		if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') c->p++;
		else break;
	}
}

static inline int expect_ch(Cur *c, char ch)
{
	skip_ws(c);
	if (c->p < c->end && *c->p == ch) { c->p++; return 0; }
	return -1;
}

/* Parses a JSON string into (out, out_len). Doesn't handle escapes; advances
 * past the closing quote. */
static int parse_str(Cur *c, const char **out, int *out_len)
{
	skip_ws(c);
	if (c->p >= c->end || *c->p != '"') return -1;
	c->p++;
	const char *start = c->p;
	while (c->p < c->end && *c->p != '"') c->p++;
	if (c->p >= c->end) return -1;
	*out = start;
	*out_len = (int)(c->p - start);
	c->p++; /* skip closing quote */
	return 0;
}

/* Parses a JSON number into a double using strtod. */
static int parse_num(Cur *c, double *out)
{
	skip_ws(c);
	char *endptr;
	double v = strtod(c->p, &endptr);
	if (endptr == c->p) return -1;
	c->p = endptr;
	*out = v;
	return 0;
}

/* Parses true / false / null. Sets *is_null=1 if null. */
static int parse_word(Cur *c, int *is_true, int *is_null)
{
	skip_ws(c);
	*is_null = 0;
	if (c->end - c->p >= 4 && memcmp(c->p, "true", 4) == 0) {
		c->p += 4; *is_true = 1; return 0;
	}
	if (c->end - c->p >= 5 && memcmp(c->p, "false", 5) == 0) {
		c->p += 5; *is_true = 0; return 0;
	}
	if (c->end - c->p >= 4 && memcmp(c->p, "null", 4) == 0) {
		c->p += 4; *is_null = 1; return 0;
	}
	return -1;
}

/* Skip a JSON value of any type (string, number, bool, null, object, array). */
static int skip_value(Cur *c)
{
	skip_ws(c);
	if (c->p >= c->end) return -1;
	char ch = *c->p;
	if (ch == '"') {
		const char *s; int n;
		return parse_str(c, &s, &n);
	}
	if (ch == 't' || ch == 'f' || ch == 'n') {
		int t, nu;
		return parse_word(c, &t, &nu);
	}
	if (ch == '-' || (ch >= '0' && ch <= '9')) {
		double d;
		return parse_num(c, &d);
	}
	if (ch == '{' || ch == '[') {
		int depth = 0;
		char open = ch;
		char close_ = (ch == '{') ? '}' : ']';
		c->p++;
		depth = 1;
		while (c->p < c->end && depth > 0) {
			char k = *c->p;
			if (k == '"') {
				const char *s; int n;
				if (parse_str(c, &s, &n) < 0) return -1;
				continue;
			}
			if (k == open)  depth++;
			else if (k == close_) depth--;
			c->p++;
		}
		return depth == 0 ? 0 : -1;
	}
	return -1;
}

/* Generic object walker: for each key, dispatch via callback.
 * keymatch(name, name_len) returns the key-id, or -1 to skip.
 * apply(c, id, p) reads the value and stores into Payload. */
static int parse_object(Cur *c, Payload *p,
                        int (*keymatch)(const char *, int),
                        int (*apply)(Cur *, int, Payload *))
{
	if (expect_ch(c, '{') < 0) return -1;
	skip_ws(c);
	if (c->p < c->end && *c->p == '}') { c->p++; return 0; }
	for (;;) {
		const char *kname; int klen;
		if (parse_str(c, &kname, &klen) < 0) return -1;
		if (expect_ch(c, ':') < 0) return -1;
		int id = keymatch(kname, klen);
		if (id >= 0) {
			if (apply(c, id, p) < 0) return -1;
		} else {
			if (skip_value(c) < 0) return -1;
		}
		skip_ws(c);
		if (c->p >= c->end) return -1;
		if (*c->p == ',') { c->p++; continue; }
		if (*c->p == '}') { c->p++; return 0; }
		return -1;
	}
}

/* ---- transaction sub-object ---- */
enum { TK_AMOUNT, TK_INSTALLMENTS, TK_REQUESTED_AT };
static int tx_key(const char *n, int l)
{
	if (l == 6  && memcmp(n, "amount", 6) == 0)        return TK_AMOUNT;
	if (l == 12 && memcmp(n, "installments", 12) == 0) return TK_INSTALLMENTS;
	if (l == 12 && memcmp(n, "requested_at", 12) == 0) return TK_REQUESTED_AT;
	return -1;
}
static int tx_apply(Cur *c, int id, Payload *p)
{
	if (id == TK_AMOUNT)        return parse_num(c, &p->tx_amount);
	if (id == TK_INSTALLMENTS)  return parse_num(c, &p->tx_installments);
	if (id == TK_REQUESTED_AT)  return parse_str(c, &p->tx_requested_at, &p->tx_requested_at_len);
	return -1;
}

/* ---- customer sub-object ---- */
enum { CK_AVG_AMOUNT, CK_TX_COUNT_24H, CK_KNOWN_MERCHANTS };
static int cust_key(const char *n, int l)
{
	if (l == 10 && memcmp(n, "avg_amount",      10) == 0) return CK_AVG_AMOUNT;
	if (l == 12 && memcmp(n, "tx_count_24h",    12) == 0) return CK_TX_COUNT_24H;
	if (l == 15 && memcmp(n, "known_merchants", 15) == 0) return CK_KNOWN_MERCHANTS;
	return -1;
}
static int cust_apply(Cur *c, int id, Payload *p)
{
	if (id == CK_AVG_AMOUNT)   return parse_num(c, &p->cust_avg_amount);
	if (id == CK_TX_COUNT_24H) return parse_num(c, &p->cust_tx_count_24h);
	if (id == CK_KNOWN_MERCHANTS) {
		if (expect_ch(c, '[') < 0) return -1;
		p->known_merchants_count = 0;
		skip_ws(c);
		if (c->p < c->end && *c->p == ']') { c->p++; return 0; }
		for (;;) {
			const char *s; int n;
			if (parse_str(c, &s, &n) < 0) return -1;
			if (p->known_merchants_count < KNOWN_MERCHANTS_CAP) {
				p->known_merchants[p->known_merchants_count]      = s;
				p->known_merchants_lens[p->known_merchants_count] = n;
				p->known_merchants_count++;
			}
			skip_ws(c);
			if (c->p >= c->end) return -1;
			if (*c->p == ',') { c->p++; continue; }
			if (*c->p == ']') { c->p++; return 0; }
			return -1;
		}
	}
	return -1;
}

/* ---- merchant sub-object ---- */
enum { MK_ID, MK_MCC, MK_AVG_AMOUNT };
static int mer_key(const char *n, int l)
{
	if (l == 2  && memcmp(n, "id",          2) == 0) return MK_ID;
	if (l == 3  && memcmp(n, "mcc",         3) == 0) return MK_MCC;
	if (l == 10 && memcmp(n, "avg_amount", 10) == 0) return MK_AVG_AMOUNT;
	return -1;
}
static int mer_apply(Cur *c, int id, Payload *p)
{
	if (id == MK_ID) return parse_str(c, &p->mer_id, &p->mer_id_len);
	if (id == MK_MCC) {
		const char *s; int n;
		if (parse_str(c, &s, &n) < 0) return -1;
		int v = 0; int valid = (n > 0);
		for (int i = 0; i < n; i++) {
			char ch = s[i];
			if (ch < '0' || ch > '9') { valid = 0; break; }
			v = v * 10 + (ch - '0');
		}
		p->mer_mcc       = v;
		p->mer_mcc_valid = valid;
		return 0;
	}
	if (id == MK_AVG_AMOUNT) return parse_num(c, &p->mer_avg_amount);
	return -1;
}

/* ---- terminal sub-object ---- */
enum { LK_IS_ONLINE, LK_CARD_PRESENT, LK_KM_FROM_HOME };
static int term_key(const char *n, int l)
{
	if (l == 9  && memcmp(n, "is_online",     9) == 0) return LK_IS_ONLINE;
	if (l == 12 && memcmp(n, "card_present", 12) == 0) return LK_CARD_PRESENT;
	if (l == 12 && memcmp(n, "km_from_home", 12) == 0) return LK_KM_FROM_HOME;
	return -1;
}
static int term_apply(Cur *c, int id, Payload *p)
{
	if (id == LK_IS_ONLINE || id == LK_CARD_PRESENT) {
		int t, nu;
		if (parse_word(c, &t, &nu) < 0) return -1;
		if (nu) return -1;
		if (id == LK_IS_ONLINE)    p->term_is_online    = t;
		else                        p->term_card_present = t;
		return 0;
	}
	if (id == LK_KM_FROM_HOME) return parse_num(c, &p->term_km_from_home);
	return -1;
}

/* ---- last_transaction sub-object ---- */
enum { LTK_TIMESTAMP, LTK_KM_FROM_CURRENT };
static int last_key(const char *n, int l)
{
	if (l == 9  && memcmp(n, "timestamp",        9) == 0) return LTK_TIMESTAMP;
	if (l == 15 && memcmp(n, "km_from_current", 15) == 0) return LTK_KM_FROM_CURRENT;
	return -1;
}
static int last_apply(Cur *c, int id, Payload *p)
{
	if (id == LTK_TIMESTAMP)        return parse_str(c, &p->last_tx_timestamp, &p->last_tx_timestamp_len);
	if (id == LTK_KM_FROM_CURRENT)  return parse_num(c, &p->last_tx_km_from_current);
	return -1;
}

/* ---- top-level keys ---- */
enum { TOP_TRANSACTION, TOP_CUSTOMER, TOP_MERCHANT, TOP_TERMINAL, TOP_LAST_TX };
static int top_key(const char *n, int l)
{
	if (l == 11 && memcmp(n, "transaction",       11) == 0) return TOP_TRANSACTION;
	if (l == 8  && memcmp(n, "customer",           8) == 0) return TOP_CUSTOMER;
	if (l == 8  && memcmp(n, "merchant",           8) == 0) return TOP_MERCHANT;
	if (l == 8  && memcmp(n, "terminal",           8) == 0) return TOP_TERMINAL;
	if (l == 16 && memcmp(n, "last_transaction", 16) == 0) return TOP_LAST_TX;
	return -1;
}
static int top_apply(Cur *c, int id, Payload *p)
{
	if (id == TOP_TRANSACTION) return parse_object(c, p, tx_key,   tx_apply);
	if (id == TOP_CUSTOMER)    return parse_object(c, p, cust_key, cust_apply);
	if (id == TOP_MERCHANT)    return parse_object(c, p, mer_key,  mer_apply);
	if (id == TOP_TERMINAL)    return parse_object(c, p, term_key, term_apply);
	if (id == TOP_LAST_TX) {
		skip_ws(c);
		if (c->p < c->end && *c->p == 'n') {
			int t, nu;
			if (parse_word(c, &t, &nu) < 0) return -1;
			if (!nu) return -1;
			p->has_last_tx = 0;
			return 0;
		}
		p->has_last_tx = 1;
		return parse_object(c, p, last_key, last_apply);
	}
	return -1;
}

int json_parse(const char *buf, int len, Payload *p)
{
	memset(p, 0, sizeof(*p));
	Cur c = { buf, buf + len };
	return parse_object(&c, p, top_key, top_apply);
}
