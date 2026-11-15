/* The cookie manager's model. See cookie_view.h. */
#include "cookie_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void au_cookie_view_init(AuCookieView *v, AuCookieJar *jar)
{
    memset(v, 0, sizeof *v);
    v->jar = jar;
    au_listview_init(&v->list);
}

static void free_list(char **l, size_t n)
{
    for (size_t i = 0; i < n; i++)
        au_free(l[i]);
    au_free(l);
}

void au_cookie_view_free(AuCookieView *v)
{
    au_listview_free(&v->list);
    free_list(v->sites, v->nsites);
    free_list(v->open, v->nopen);
    memset(v, 0, sizeof *v);
}

static int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool has(char **l, size_t n, const char *s)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(l[i], s))
            return true;
    return false;
}

static void add_unique(char ***l, size_t *n, const char *s)
{
    if (has(*l, *n, s))
        return;
    *l = au_realloc(*l, (*n + 1) * sizeof **l);
    (*l)[(*n)++] = au_strndup(s, strlen(s));
}

static bool is_open(const AuCookieView *v, const char *site) { return has(v->open, v->nopen, site); }

void au_cookie_format_time(int64_t t, char *out, size_t size)
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t secs = t - days * 86400;
    /* Civil date from days since 1970 (Howard Hinnant's algorithm). */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d", (int)y, (int)m, (int)d, (int)(secs / 3600),
             (int)(secs / 60 % 60));
}

static const char *rule_label(AuCookieRule r)
{
    switch (r) {
    case AU_COOKIE_RULE_ALLOW:
        return "always allowed";
    case AU_COOKIE_RULE_SESSION:
        return "session only";
    case AU_COOKIE_RULE_BLOCK:
        return "blocked";
    default:
        return "";
    }
}

void au_cookie_view_refresh(AuCookieView *v, int64_t now)
{
    char *keep = NULL;
    const char *sel = au_cookie_view_site(v, v->list.selected);
    bool sel_site = v->list.selected >= 0 && v->list.selected < v->list.n &&
                    (v->list.rows[v->list.selected].id & AU_COOKIE_ROW_SITE);
    if (sel)
        keep = au_strndup(sel, strlen(sel));
    free_list(v->sites, v->nsites);
    v->sites = NULL;
    v->nsites = 0;
    AuBuf k;
    au_buf_init(&k);
    size_t n = au_cookie_jar_count(v->jar);
    for (size_t i = 0; i < n; i++) {
        const AuCookie *c = au_cookie_jar_at(v->jar, i);
        if (c->expires && c->expires <= now)
            continue;
        au_cookie_site_key(c->domain, &k);
        add_unique(&v->sites, &v->nsites, au_buf_cstr(&k));
    }
    for (size_t i = 0; i < au_cookie_rules_count(v->jar); i++)
        add_unique(&v->sites, &v->nsites, au_cookie_rules_at(v->jar, i, NULL));
    if (v->nsites > 1)
        qsort(v->sites, v->nsites, sizeof *v->sites, str_cmp);

    au_listview_clear(&v->list);
    int new_sel = -1;
    AuBuf detail;
    au_buf_init(&detail);
    for (size_t s = 0; s < v->nsites; s++) {
        const char *site = v->sites[s];
        size_t count = 0;
        for (size_t i = 0; i < n; i++) {
            const AuCookie *c = au_cookie_jar_at(v->jar, i);
            if (c->expires && c->expires <= now)
                continue;
            au_cookie_site_key(c->domain, &k);
            if (!strcmp(au_buf_cstr(&k), site))
                count++;
        }
        au_buf_clear(&detail);
        au_buf_printf(&detail, "%u cookie%s", (unsigned)count, count == 1 ? "" : "s");
        AuCookieRule r = au_cookie_jar_rule(v->jar, site);
        if (r != AU_COOKIE_RULE_NONE)
            au_buf_printf(&detail, ", %s", rule_label(r));
        bool open = is_open(v, site);
        if (keep && sel_site && !strcmp(keep, site))
            new_sel = v->list.n;
        au_listview_add(&v->list, AU_ROW_FOLDER, 0, open, site, au_buf_cstr(&detail), AU_COOKIE_ROW_SITE | (uint32_t)s);
        if (!open)
            continue;
        for (size_t i = 0; i < n; i++) {
            const AuCookie *c = au_cookie_jar_at(v->jar, i);
            if (c->expires && c->expires <= now)
                continue;
            au_cookie_site_key(c->domain, &k);
            if (strcmp(au_buf_cstr(&k), site))
                continue;
            au_buf_clear(&detail);
            au_buf_printf(&detail, "%s%s%s", c->host_only ? "" : ".", c->domain, c->path);
            if (c->secure)
                au_buf_puts(&detail, "  Secure");
            if (c->http_only)
                au_buf_puts(&detail, "  HttpOnly");
            if (c->expires) {
                char when[32];
                au_cookie_format_time(c->expires + v->utc_offset, when, sizeof when);
                au_buf_printf(&detail, "  until %s", when);
            } else {
                au_buf_puts(&detail, "  session");
            }
            au_listview_add(&v->list, AU_ROW_LINK, 1, false, c->name[0] ? c->name : "(no name)",
                            au_buf_cstr(&detail), (uint32_t)i);
        }
    }
    if (new_sel < 0 && keep && !sel_site) {
        /* A removed cookie: select its site. */
        for (int r = 0; r < v->list.n; r++)
            if ((v->list.rows[r].id & AU_COOKIE_ROW_SITE) && !strcmp(v->list.rows[r].text, keep)) {
                new_sel = r;
                break;
            }
    }
    v->list.selected = new_sel < v->list.n ? new_sel : -1;
    au_buf_free(&detail);
    au_buf_free(&k);
    au_free(keep);
    int max = au_listview_content_height(&v->list) - v->list.height;
    if (v->list.scroll > (max > 0 ? max : 0))
        v->list.scroll = max > 0 ? max : 0;
}

const char *au_cookie_view_site(const AuCookieView *v, int row)
{
    if (row < 0 || row >= v->list.n)
        return NULL;
    uint32_t id = v->list.rows[row].id;
    if (id & AU_COOKIE_ROW_SITE) {
        size_t s = id & ~AU_COOKIE_ROW_SITE;
        return s < v->nsites ? v->sites[s] : NULL;
    }
    for (int r = row; r >= 0; r--)
        if (v->list.rows[r].id & AU_COOKIE_ROW_SITE)
            return au_cookie_view_site(v, r);
    return NULL;
}

const AuCookie *au_cookie_view_cookie(const AuCookieView *v, int row)
{
    if (row < 0 || row >= v->list.n || (v->list.rows[row].id & AU_COOKIE_ROW_SITE))
        return NULL;
    return au_cookie_jar_at(v->jar, v->list.rows[row].id);
}

void au_cookie_view_toggle(AuCookieView *v, int row, int64_t now)
{
    if (row < 0 || row >= v->list.n || !(v->list.rows[row].id & AU_COOKIE_ROW_SITE))
        return;
    const char *site = au_cookie_view_site(v, row);
    if (!site)
        return;
    bool found = false;
    for (size_t i = 0; i < v->nopen; i++)
        if (!strcmp(v->open[i], site)) {
            au_free(v->open[i]);
            memmove(v->open + i, v->open + i + 1, (v->nopen - i - 1) * sizeof *v->open);
            v->nopen--;
            found = true;
            break;
        }
    if (!found)
        add_unique(&v->open, &v->nopen, site);
    v->list.selected = row;
    au_cookie_view_refresh(v, now);
}

static const char *const same_site_names[] = {"not set (Lax)", "None", "Lax", "Strict"};

void au_cookie_view_describe(const AuCookieView *v, int row, AuBuf *out, int64_t now)
{
    au_buf_clear(out);
    const AuCookie *c = au_cookie_view_cookie(v, row);
    char when[32];
    if (!c) {
        const char *site = au_cookie_view_site(v, row);
        if (!site)
            return;
        size_t count = 0;
        AuBuf k;
        au_buf_init(&k);
        for (size_t i = 0; i < au_cookie_jar_count(v->jar); i++) {
            const AuCookie *o = au_cookie_jar_at(v->jar, i);
            au_cookie_site_key(o->domain, &k);
            if ((!o->expires || o->expires > now) && !strcmp(au_buf_cstr(&k), site))
                count++;
        }
        au_buf_free(&k);
        AuCookieRule r = au_cookie_jar_rule(v->jar, site);
        au_buf_printf(out, "Site: %s\nCookies: %u\nRule: %s\n", site, (unsigned)count,
                      r == AU_COOKIE_RULE_NONE ? "none (the cookie preferences apply)" : rule_label(r));
        return;
    }
    au_buf_printf(out, "Name: %s\nValue: %s\n", c->name, c->value);
    au_buf_printf(out, "%s: %s\n", c->host_only ? "Host" : "Domain", c->domain);
    au_buf_printf(out, "Path: %s\n", c->path);
    if (c->expires) {
        au_cookie_format_time(c->expires + v->utc_offset, when, sizeof when);
        au_buf_printf(out, "Expires: %s\n", when);
    } else {
        au_buf_puts(out, "Expires: at the end of the session\n");
    }
    au_cookie_format_time(c->creation + v->utc_offset, when, sizeof when);
    au_buf_printf(out, "Created: %s\n", when);
    au_cookie_format_time(c->last_access + v->utc_offset, when, sizeof when);
    au_buf_printf(out, "Last used: %s\n", when);
    au_buf_printf(out, "Secure: %s\nHttpOnly: %s\nSameSite: %s\n", c->secure ? "yes" : "no",
                  c->http_only ? "yes" : "no", same_site_names[c->same_site & 3]);
    if (c->partitioned)
        au_buf_printf(out, "Partitioned: under %s\n", c->partition);
}

size_t au_cookie_view_remove(AuCookieView *v, int row, int64_t now)
{
    size_t removed = 0;
    const AuCookie *c = au_cookie_view_cookie(v, row);
    if (c) {
        au_cookie_jar_remove_at(v->jar, v->list.rows[row].id);
        removed = 1;
    } else {
        const char *s = au_cookie_view_site(v, row);
        if (!s)
            return 0;
        char *site = au_strndup(s, strlen(s));
        AuBuf k;
        au_buf_init(&k);
        for (size_t i = 0; i < au_cookie_jar_count(v->jar);) {
            au_cookie_site_key(au_cookie_jar_at(v->jar, i)->domain, &k);
            if (!strcmp(au_buf_cstr(&k), site)) {
                au_cookie_jar_remove_at(v->jar, i);
                removed++;
            } else {
                i++;
            }
        }
        au_buf_free(&k);
        au_free(site);
    }
    au_cookie_view_refresh(v, now);
    return removed;
}

void au_cookie_view_remove_all(AuCookieView *v, int64_t now)
{
    au_cookie_jar_clear(v->jar);
    v->list.selected = -1;
    au_cookie_view_refresh(v, now);
}

void au_cookie_view_set_rule(AuCookieView *v, int row, AuCookieRule rule, int64_t now)
{
    const char *s = au_cookie_view_site(v, row);
    if (!s)
        return;
    char *site = au_strndup(s, strlen(s));
    au_cookie_jar_set_rule(v->jar, site, rule);
    au_free(site);
    au_cookie_view_refresh(v, now);
}
