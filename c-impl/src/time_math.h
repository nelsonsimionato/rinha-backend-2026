#ifndef TIME_MATH_H
#define TIME_MATH_H

#include "compat.h"

/* Parse a fixed-format UTC ISO-8601 timestamp like "2026-03-11T18:45:53Z".
 * Returns 1 on success, 0 on length/format mismatch (no allocation, no time_t). */
int parse_iso(const char *b, int len,
              int *year, int *month, int *day,
              int *hour, int *minute, int *second);

/* Mon=0..Sun=6 via Zeller's congruence. Byte-for-byte match to Go's specDayOfWeek. */
int spec_day_of_week(int year, int month, int day);

/* Days since 0000-03-01 (Howard Hinnant variant). Anchor arbitrary;
 * differences are exact for any proleptic Gregorian dates. */
int days_from_epoch(int year, int month, int day);

/* Total minutes between two timestamps (signed). Matches Go's minutesBetween. */
int64_t minutes_between(int y1, int mo1, int d1, int h1, int mi1, int s1,
                        int y2, int mo2, int d2, int h2, int mi2, int s2);

#endif /* TIME_MATH_H */
