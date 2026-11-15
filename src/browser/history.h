/* Global browsing history and the calendar helpers it groups by.
 * Portable C99.
 *
 * Times (AuTime) are seconds since 1970-01-01 00:00 in local time: the
 * platform converts its clock (Mac OS keeps local time already). Days for
 * grouping are local days.
 *
 * Entries are kept most recent first, one per URL. A visit moves the URL
 * to the front and counts it. The list is capped (oldest dropped first)
 * and entries older than the days-to-keep preference are expired.
 *
 * File format, compacted on every save (written most recent first,
 * deduplicated, capped, expired):
 *     # Aura history 1
 *     <last visit>\t<visit count>\t<url>\t<title>
 * one entry per line; tabs and line breaks in titles become spaces. */
#ifndef AURA_BROWSER_HISTORY_H
#define AURA_BROWSER_HISTORY_H

#include "../core/core.h"

typedef int64_t AuTime;

/* ---- Calendar ---- */
/* The day number (days since 1970-01-01, local) of a time. */
int64_t au_day_of(AuTime t);
/* The civil date of a day number: year, month 1..12, day 1..31, weekday
 * 0 (Sunday)..6. */
void au_date_of_day(int64_t day, int *year, int *month, int *mday, int *weekday);
/* The time for a civil date at 00:00. */
AuTime au_time_of_date(int year, int month, int mday);
/* A heading for a day relative to today: "Today", "Yesterday", a weekday
 * name within the last week, else "Monday, 3 November 2026". */
void au_day_label(int64_t day, int64_t today, char *out, size_t cap);

/* Appends v in decimal (the files use it for times). */
void au_put_i64(AuBuf *b, int64_t v);

/* ---- History ---- */

typedef struct AuHistoryEntry {
    char *url, *title;
    AuTime last_visit;
    uint32_t visits;
} AuHistoryEntry;

typedef struct AuHistory {
    AuHistoryEntry *e; /* most recent first */
    size_t n, cap;
    size_t max_entries;
    uint32_t generation; /* bumped on every change, for menus and saving */
} AuHistory;

#define AU_HISTORY_DEFAULT_MAX 1000

void au_history_init(AuHistory *h, size_t max_entries);
void au_history_free(AuHistory *h);
/* Records a visit at time now. title may be NULL (keeps the old one). */
void au_history_visit(AuHistory *h, const char *url, const char *title, AuTime now);
void au_history_set_title(AuHistory *h, const char *url, const char *title);
/* Index of url, or -1. */
long au_history_find(const AuHistory *h, const char *url);
void au_history_remove(AuHistory *h, size_t i);
void au_history_clear(AuHistory *h);
/* Drops entries last visited more than days days before now (days <= 0
 * keeps everything). */
void au_history_expire(AuHistory *h, AuTime now, int days);

/* Day groups for the history window: entries are already most recent
 * first, so a group is a run of entries on the same local day. */
typedef struct AuHistoryGroup {
    int64_t day;
    size_t first, count;
} AuHistoryGroup;
/* Fills up to max groups and returns how many there are in all. */
size_t au_history_groups(const AuHistory *h, AuHistoryGroup *out, size_t max);

/* The best completion for typed address-field text: the most visited
 * entry whose URL, without scheme and "www.", starts with typed (ASCII
 * case-insensitive). NULL when none. */
const AuHistoryEntry *au_history_complete(const AuHistory *h, const char *typed);

void au_history_serialize(const AuHistory *h, AuBuf *out);
/* Merges a history file into h (which is usually empty). Returns false
 * when the file looks damaged (fewer good lines than bad, or a wrong
 * header); good lines are still read. */
bool au_history_parse(AuHistory *h, const char *data, size_t len);

#endif
