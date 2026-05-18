#include "time_math.h"

static inline int atoi2(const char *b)
{
	return (b[0] - '0') * 10 + (b[1] - '0');
}

static inline int atoi4(const char *b)
{
	return (b[0] - '0') * 1000 +
	       (b[1] - '0') * 100 +
	       (b[2] - '0') * 10  +
	       (b[3] - '0');
}

int parse_iso(const char *b, int len,
              int *year, int *month, int *day,
              int *hour, int *minute, int *second)
{
	if (len < 19) return 0;
	*year   = atoi4(b);
	*month  = atoi2(b + 5);
	*day    = atoi2(b + 8);
	*hour   = atoi2(b + 11);
	*minute = atoi2(b + 14);
	*second = atoi2(b + 17);
	return 1;
}

int spec_day_of_week(int y, int mo, int d)
{
	int k_yr, j_cy, h;
	if (mo < 3) { mo += 12; y--; }
	k_yr = y % 100;
	j_cy = y / 100;
	/* Zeller: 0=Sat, 1=Sun, 2=Mon, ..., 6=Fri.
	 * spec = (h + 5) mod 7 → Mon=0..Sun=6.
	 * Variables renamed from textbook (K/J → k_yr/j_cy) to avoid macro
	 * collision with K=5 in compat.h. */
	h = (d + 13 * (mo + 1) / 5 + k_yr + k_yr / 4 + j_cy / 4 + 5 * j_cy) % 7;
	return (h + 5) % 7;
}

int days_from_epoch(int y, int m, int d)
{
	int era, yoe, moe, doy, doe;
	if (m <= 2) y--;
	era = y / 400;
	yoe = y - era * 400;
	moe = (m > 2) ? (m - 3) : (m + 9);
	doy = (153 * moe + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe;
}

int64_t minutes_between(int y1, int mo1, int d1, int h1, int mi1, int s1,
                        int y2, int mo2, int d2, int h2, int mi2, int s2)
{
	int64_t day_diff = (int64_t)(days_from_epoch(y1, mo1, d1) -
	                              days_from_epoch(y2, mo2, d2));
	int64_t secs = day_diff * 86400 +
	               (int64_t)((h1 - h2) * 3600 + (mi1 - mi2) * 60 + (s1 - s2));
	return secs / 60;
}
