/* Global browsing history. See history.h. */
#include "history.h"
#include "url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Calendar (proleptic Gregorian; Howard Hinnant's civil algorithms) ---- */

static int64_t floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

int64_t au_day_of(AuTime t)
{
    return floordiv(t, 86400);
}

void au_date_of_day(int64_t z, int *year, int *month, int *mday, int *weekday)
{
    if (weekday) {
        int64_t w = (z + 4) % 7; /* 1970-01-01 was a Thursday */
        if (w < 0)
            w += 7;
        *weekday = (int)w;
    }
    z += 719468;
    int64_t era = floordiv(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (year)
        *year = (int)(y + (m <= 2));
    if (month)
        *month = (int)m;
    if (mday)
        *mday = (int)d;
}

AuTime au_time_of_date(int year, int month, int mday)
{
    int64_t y = year - (month <= 2);
    int64_t era = floordiv(y, 400);
    int64_t yoe = y - era * 400;
    int64_t mp = month > 2 ? month - 3 : month + 9;
    int64_t doy = (153 * mp + 2) / 5 + mday - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400;
}

static const char *const weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                     "July",    "August",   "September", "October", "November", "December"};

void au_day_label(int64_t day, int64_t today, char *out, size_t cap)
{
    int y, m, d, wd;
    au_date_of_day(day, &y, &m, &d, &wd);
    if (day == today)
        snprintf(out, cap, "Today");
    else if (day == today - 1)
        snprintf(out, cap, "Yesterday");
    else if (day < today && day > today - 7)
        snprintf(out, cap, "%s", weekdays[wd]);
    else
        snprintf(out, cap, "%s, %d %s %d", weekdays[wd], d, months[m - 1], y);
}

/* ---- History ---- */

void au_history_init(AuHistory *h, size_t max_entries)
{
    memset(h, 0, sizeof *h);
    h->max_entries = max_entries ? max_entries : AU_HISTORY_DEFAULT_MAX;
}

static void free_entry(AuHistoryEntry *e)
{
    au_free(e->url);
    au_free(e->title);
}

void au_history_free(AuHistory *h)
{
    for (size_t i = 0; i < h->n; i++)
        free_entry(&h->e[i]);
    au_free(h->e);
    memset(h, 0, sizeof *h);
}

long au_history_find(const AuHistory *h, const char *url)
{
    for (size_t i = 0; i < h->n; i++)
        if (!strcmp(h->e[i].url, url))
            return (long)i;
    return -1;
}

void au_history_remove(AuHistory *h, size_t i)
{
    if (i >= h->n)
        return;
    free_entry(&h->e[i]);
    memmove(&h->e[i], &h->e[i + 1], (h->n - i - 1) * sizeof *h->e);
    h->n--;
    h->generation++;
}

void au_history_clear(AuHistory *h)
{
    for (size_t i = 0; i < h->n; i++)
        free_entry(&h->e[i]);
    h->n = 0;
    h->generation++;
}

static char *clean_title(const char *t)
{
    if (!t)
        t = "";
    char *c = au_strndup(t, strlen(t));
    for (char *p = c; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r')
            *p = ' ';
    return c;
}

static void cap(AuHistory *h)
{
    while (h->n > h->max_entries)
        free_entry(&h->e[--h->n]);
}

void au_history_visit(AuHistory *h, const char *url, const char *title, AuTime now)
{
    long i = au_history_find(h, url);
    AuHistoryEntry e;
    if (i >= 0) {
        e = h->e[i];
        memmove(&h->e[1], &h->e[0], (size_t)i * sizeof *h->e);
        if (title && *title) {
            au_free(e.title);
            e.title = clean_title(title);
        }
        e.visits++;
    } else {
        if (h->n == h->cap) {
            h->cap = h->cap ? h->cap * 2 : 64;
            h->e = au_realloc(h->e, h->cap * sizeof *h->e);
        }
        memmove(&h->e[1], &h->e[0], h->n * sizeof *h->e);
        h->n++;
        e.url = clean_title(url);
        e.title = clean_title(title);
        e.visits = 1;
    }
    e.last_visit = now;
    h->e[0] = e;
    cap(h);
    h->generation++;
}

void au_history_set_title(AuHistory *h, const char *url, const char *title)
{
    long i = au_history_find(h, url);
    if (i < 0 || !title)
        return;
    if (!strcmp(h->e[i].title, title))
        return;
    au_free(h->e[i].title);
    h->e[i].title = clean_title(title);
    h->generation++;
}

void au_history_expire(AuHistory *h, AuTime now, int days)
{
    if (days <= 0)
        return;
    AuTime limit = now - (AuTime)days * 86400;
    size_t before = h->n;
    while (h->n && h->e[h->n - 1].last_visit < limit)
        free_entry(&h->e[--h->n]);
    if (h->n != before)
        h->generation++;
}

size_t au_history_groups(const AuHistory *h, AuHistoryGroup *out, size_t max)
{
    size_t ng = 0;
    for (size_t i = 0; i < h->n;) {
        int64_t day = au_day_of(h->e[i].last_visit + h->utc_offset);
        size_t j = i + 1;
        while (j < h->n && au_day_of(h->e[j].last_visit + h->utc_offset) == day)
            j++;
        if (ng < max) {
            out[ng].day = day;
            out[ng].first = i;
            out[ng].count = j - i;
        }
        ng++;
        i = j;
    }
    return ng;
}

const AuHistoryEntry *au_history_complete(const AuHistory *h, const char *typed)
{
    size_t tn = strlen(typed);
    if (tn == 0)
        return NULL;
    /* Typed text with a scheme matches whole URLs; else the match key. */
    bool whole = au_url_scheme_len(typed, tn) != 0;
    const char *t = typed;
    if (!whole && tn > 4 && !strncmp(t, "www.", 4))
        t += 4, tn -= 4;
    const AuHistoryEntry *best = NULL;
    for (size_t i = 0; i < h->n; i++) {
        const AuHistoryEntry *e = &h->e[i];
        size_t kn;
        const char *k = whole ? e->url : au_url_match_key(e->url, &kn);
        if (whole)
            kn = strlen(k);
        if (kn > tn && au_ieq(k, tn, t, tn) && (!best || e->visits > best->visits))
            best = e;
    }
    return best;
}

/* Without printf's %lld, which newlib builds may leave out. */
void au_put_i64(AuBuf *b, int64_t v)
{
    char tmp[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        au_buf_putc(b, '-');
    while (n)
        au_buf_putc(b, tmp[--n]);
}

void au_history_serialize(const AuHistory *h, AuBuf *out)
{
    au_buf_puts(out, "# Aura history 1\n");
    for (size_t i = 0; i < h->n; i++) {
        const AuHistoryEntry *e = &h->e[i];
        au_put_i64(out, e->last_visit);
        au_buf_printf(out, "\t%lu\t", (unsigned long)e->visits);
        au_buf_puts(out, e->url);
        au_buf_putc(out, '\t');
        au_buf_puts(out, e->title);
        au_buf_putc(out, '\n');
    }
}

static bool parse_num(const char *s, size_t n, int64_t *out)
{
    if (n == 0 || n > 19)
        return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (n == 1)
            return false;
    }
    int64_t v = 0;
    for (; i < n; i++) {
        if (!au_is_digit((unsigned char)s[i]))
            return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

static int cmp_recent(const void *a, const void *b)
{
    const AuHistoryEntry *x = a, *y = b;
    if (x->last_visit != y->last_visit)
        return x->last_visit > y->last_visit ? -1 : 1;
    return 0;
}

bool au_history_parse(AuHistory *h, const char *data, size_t len)
{
    size_t good = 0, bad = 0;
    bool header = false;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && data[e] != '\n' && data[e] != '\r')
            e++;
        const char *line = data + i;
        size_t n = e - i;
        i = e;
        while (i < len && (data[i] == '\n' || data[i] == '\r'))
            i++;
        if (n == 0)
            continue;
        if (line[0] == '#') {
            if (n >= 16 && !strncmp(line, "# Aura history 1", 16))
                header = true;
            continue;
        }
        const char *f[4];
        size_t fl[4];
        int nf = 0;
        size_t s = 0;
        for (size_t k = 0; k <= n && nf < 4; k++) {
            if (k == n || (line[k] == '\t' && nf < 3)) {
                f[nf] = line + s;
                fl[nf] = k - s;
                nf++;
                s = k + 1;
            }
        }
        int64_t when, visits;
        if (nf < 3 || memchr(line, 0, n) || !parse_num(f[0], fl[0], &when) || !parse_num(f[1], fl[1], &visits) ||
            fl[2] == 0 || visits < 0) {
            bad++;
            continue;
        }
        good++;
        char *url = au_strndup(f[2], fl[2]);
        char *title = nf > 3 ? au_strndup(f[3], fl[3]) : au_strndup("", 0);
        long at = au_history_find(h, url);
        if (at >= 0) {
            AuHistoryEntry *x = &h->e[at];
            if (when > x->last_visit) {
                x->last_visit = when;
                au_free(x->title);
                x->title = title;
                title = NULL;
            }
            x->visits += (uint32_t)visits;
            au_free(url);
            au_free(title);
            continue;
        }
        if (h->n == h->cap) {
            h->cap = h->cap ? h->cap * 2 : 64;
            h->e = au_realloc(h->e, h->cap * sizeof *h->e);
        }
        AuHistoryEntry *x = &h->e[h->n++];
        x->url = url;
        x->title = title;
        x->last_visit = when;
        x->visits = visits > 0xFFFFFFF ? 0xFFFFFFF : (uint32_t)visits;
    }
    /* qsort is not stable; equal times keep an arbitrary order, which is
     * harmless for history. */
    if (h->n > 1)
        qsort(h->e, h->n, sizeof *h->e, cmp_recent);
    cap(h);
    h->generation++;
    if (len > 0 && !header)
        return false;
    return good >= bad;
}
