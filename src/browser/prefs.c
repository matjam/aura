/* Browser preferences. See prefs.h. */
#include "prefs.h"
#include "credits.h"
#include "scriptmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { T_STR, T_BOOL, T_INT, T_ENUM } Kind;

typedef struct Key {
    const char *name;
    Kind kind;
    size_t off;
    int min, max;              /* T_INT range */
    const char *const *names;  /* T_ENUM names, NULL-terminated */
} Key;

static const char *const startup_names[] = {"home", "blank", "restore", NULL};
static const char *const tabpos_names[] = {"top", "left", NULL};
static const char *const cookie_names[] = {"all", "same-site", "none", NULL};
static const char *const block_update_names[] = {"daily", "weekly", "never", NULL};

#define S(n, f) {n, T_STR, offsetof(AuPrefs, f), 0, 0, NULL}
#define B(n, f) {n, T_BOOL, offsetof(AuPrefs, f), 0, 0, NULL}
#define I(n, f, lo, hi) {n, T_INT, offsetof(AuPrefs, f), lo, hi, NULL}
#define E(n, f, names) {n, T_ENUM, offsetof(AuPrefs, f), 0, 0, names}

static const Key keys[] = {
    S("home_page", home_page),
    E("startup", startup, startup_names),
    E("tabs.position", tab_position, tabpos_names),
    I("tabs.column_width", tab_column_width, AU_TAB_COLUMN_MIN, AU_TAB_COLUMN_MAX),
    S("search.template", search_template),
    S("fonts.serif", font_serif),
    S("fonts.sans_serif", font_sans),
    S("fonts.monospace", font_mono),
    I("fonts.size", font_size, 6, 72),
    I("fonts.size_monospace", font_size_mono, 6, 72),
    I("fonts.minimum_size", font_size_min, 0, 36),
    B("content.javascript", javascript),
    I("content.script_memory_kb", script_memory_kb, 0, (int)AU_SCRIPT_MEM_CUSTOM_MAX_KB),
    I("content.worker_memory_kb", worker_memory_kb, 0, (int)AU_SCRIPT_MEM_CUSTOM_MAX_KB),
    B("content.images", load_images),
    B("content.block_popups", block_popups),
    E("privacy.cookies", cookies, cookie_names),
    B("privacy.block_third_party_cookies", block_third_party_cookies),
    B("privacy.do_not_track", send_dnt),
    B("privacy.global_privacy_control", send_gpc),
    B("privacy.https_first", https_first),
    B("security.legacy_tls", legacy_tls),
    B("privacy.block_trackers", block_trackers),
    B("privacy.warn_app_downloads", warn_app_downloads),
    B("privacy.clear_history_on_quit", clear_history_on_quit),
    B("privacy.clear_cookies_on_quit", clear_cookies_on_quit),
    B("privacy.clear_cache_on_quit", clear_cache_on_quit),
    B("history.enabled", history_enabled),
    I("history.days", history_days, 0, 3650),
    S("proxy.http.host", http_proxy_host),
    I("proxy.http.port", http_proxy_port, 0, 65535),
    S("proxy.https.host", https_proxy_host),
    I("proxy.https.port", https_proxy_port, 0, 65535),
    S("network.user_agent", user_agent),
    I("cache.memory_kb", memory_cache_kb, 0, 1 << 20),
    I("cache.disk_kb", disk_cache_kb, 0, 1 << 22),
    I("cache.documents", document_cache_pages, 0, 16),
    I("cache.documents_kb", document_cache_kb, 0, 1 << 20),
    S("downloads.folder", download_folder),
    B("downloads.decode", decode_downloads),
    I("window.width", window_width, 0, 8192),
    I("window.height", window_height, 0, 8192),
    S("zoom.sites", zoom_sites),
    S("cookies.sites", cookie_sites),
    B("links.external_new_window", external_new_window),
    S("blocking.lists", block_lists),
    S("blocking.custom", block_custom),
    S("blocking.allow", block_allow),
    S("blocking.block", block_deny),
    S("blocking.sites_off", block_sites_off),
    E("blocking.update", block_update, block_update_names),
};

#define NKEYS (sizeof keys / sizeof keys[0])

static char **str_field(AuPrefs *p, const Key *k) { return (char **)((char *)p + k->off); }
static bool *bool_field(AuPrefs *p, const Key *k) { return (bool *)((char *)p + k->off); }
/* Enum fields are accessed as int; this fails to compile where enums are
 * not int-sized (-fshort-enums). */
typedef char enum_is_int[(sizeof(AuStartup) == sizeof(int) && sizeof(AuTabPosition) == sizeof(int) &&
                          sizeof(AuCookiePolicy) == sizeof(int))
                             ? 1
                             : -1];
static int *int_field(AuPrefs *p, const Key *k) { return (int *)((char *)p + k->off); }

void au_prefs_set_str(char **field, const char *value)
{
    char *v = au_strndup(value ? value : "", value ? strlen(value) : 0);
    au_free(*field);
    *field = v;
}

void au_prefs_init(AuPrefs *p)
{
    memset(p, 0, sizeof *p);
    for (size_t i = 0; i < NKEYS; i++)
        if (keys[i].kind == T_STR)
            *str_field(p, &keys[i]) = au_strndup("", 0);
    au_prefs_set_str(&p->home_page, "about:home");
    p->startup = AU_STARTUP_HOME;
    p->tab_position = AU_TABS_TOP;
    p->tab_column_width = 170;
    au_prefs_set_str(&p->search_template, "https://html.duckduckgo.com/html/?q=%s");
    au_prefs_set_str(&p->font_serif, "Times New Roman");
    au_prefs_set_str(&p->font_sans, "Helvetica");
    au_prefs_set_str(&p->font_mono, "Courier New");
    p->font_size = 16;
    p->font_size_mono = 13;
    p->font_size_min = 0;
    p->javascript = true;
    p->load_images = true;
    p->block_popups = true;
    p->cookies = AU_COOKIES_ALL;
    p->block_third_party_cookies = true;
    p->send_dnt = false;
    p->send_gpc = true;
    p->https_first = true;
    p->legacy_tls = true;
    p->block_trackers = true;
    au_prefs_set_str(&p->block_lists, "light");
    p->block_update = AU_BLOCK_UPDATE_DAILY;
    p->warn_app_downloads = true;
    p->history_enabled = true;
    p->history_days = 30;
    p->http_proxy_port = 8080;
    p->https_proxy_port = 8080;
    /* Empty: the default (au_prefs_user_agent), so a new Aura version
     * sends its own string rather than one saved by an older version. */
    au_prefs_set_str(&p->user_agent, "");
    /* 8 MB: the HTTP cache keeps entries up to a quarter of it, and a
     * Wikipedia article's HTML (1.3 MB) and script bundle must fit for
     * Back to be served from memory. The image budget per page is the
     * same figure. */
    p->memory_cache_kb = 8192;
    p->disk_cache_kb = 20480;
    /* Back and Forward keep up to 3 documents per tab, 24 MB in all: a
     * Wikipedia article is estimated at 12 to 15 MB, most of it its
     * script heap, so one or two fit. The Mac shell also drops kept
     * documents while the free heap is low. */
    p->document_cache_pages = 3;
    p->document_cache_kb = 24576;
}

void au_prefs_free(AuPrefs *p)
{
    for (size_t i = 0; i < NKEYS; i++)
        if (keys[i].kind == T_STR)
            au_free(*str_field(p, &keys[i]));
    for (size_t i = 0; i < p->nextra; i++) {
        au_free(p->extra[i].key);
        au_free(p->extra[i].value);
    }
    au_free(p->extra);
    memset(p, 0, sizeof *p);
}

void au_prefs_copy(AuPrefs *dst, const AuPrefs *src)
{
    *dst = *src;
    for (size_t i = 0; i < NKEYS; i++) {
        if (keys[i].kind == T_STR) {
            char **f = str_field(dst, &keys[i]);
            *f = au_strndup(*f, strlen(*f));
        }
    }
    dst->extra = src->nextra ? au_alloc(src->nextra * sizeof *dst->extra) : NULL;
    for (size_t i = 0; i < src->nextra; i++) {
        dst->extra[i].key = au_strndup(src->extra[i].key, strlen(src->extra[i].key));
        dst->extra[i].value = au_strndup(src->extra[i].value, strlen(src->extra[i].value));
    }
}

static const Key *find_key(const char *name)
{
    for (size_t i = 0; i < NKEYS; i++)
        if (!strcmp(keys[i].name, name))
            return &keys[i];
    return NULL;
}

static bool parse_int(const char *s, long *out)
{
    if (!*s)
        return false;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end)
        return false;
    *out = v;
    return true;
}

bool au_prefs_set(AuPrefs *p, const char *key, const char *value)
{
    const Key *k = find_key(key);
    if (!k) {
        for (size_t i = 0; i < p->nextra; i++) {
            if (!strcmp(p->extra[i].key, key)) {
                au_prefs_set_str(&p->extra[i].value, value);
                return false;
            }
        }
        p->extra = au_realloc(p->extra, (p->nextra + 1) * sizeof *p->extra);
        p->extra[p->nextra].key = au_strndup(key, strlen(key));
        p->extra[p->nextra].value = au_strndup(value, strlen(value));
        p->nextra++;
        return false;
    }
    switch (k->kind) {
    case T_STR:
        au_prefs_set_str(str_field(p, k), value);
        break;
    case T_BOOL:
        if (!strcmp(value, "true") || !strcmp(value, "1") || !strcmp(value, "yes"))
            *bool_field(p, k) = true;
        else if (!strcmp(value, "false") || !strcmp(value, "0") || !strcmp(value, "no"))
            *bool_field(p, k) = false;
        break;
    case T_INT: {
        long v;
        if (parse_int(value, &v)) {
            if (v < k->min)
                v = k->min;
            if (v > k->max)
                v = k->max;
            *int_field(p, k) = (int)v;
            /* 0 is Automatic; any other script memory limit has a floor */
            p->script_memory_kb = au_script_memory_clamp(p->script_memory_kb);
            p->worker_memory_kb = au_script_memory_clamp(p->worker_memory_kb);
        }
        break;
    }
    case T_ENUM:
        for (int i = 0; k->names[i]; i++)
            if (!strcmp(k->names[i], value))
                *int_field(p, k) = i;
        break;
    }
    return true;
}

bool au_prefs_get(const AuPrefs *p, const char *key, AuBuf *out)
{
    const Key *k = find_key(key);
    AuPrefs *q = (AuPrefs *)p; /* the accessors do not write */
    if (!k) {
        for (size_t i = 0; i < p->nextra; i++) {
            if (!strcmp(p->extra[i].key, key)) {
                au_buf_puts(out, p->extra[i].value);
                return true;
            }
        }
        return false;
    }
    switch (k->kind) {
    case T_STR: au_buf_puts(out, *str_field(q, k)); break;
    case T_BOOL: au_buf_puts(out, *bool_field(q, k) ? "true" : "false"); break;
    case T_INT: au_buf_printf(out, "%d", *int_field(q, k)); break;
    case T_ENUM: {
        int v = *int_field(q, k), n = 0;
        while (k->names[n])
            n++;
        au_buf_puts(out, k->names[v >= 0 && v < n ? v : 0]);
        break;
    }
    }
    return true;
}

static void put_escaped(AuBuf *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            au_buf_puts(b, "\\n");
        else if (*s == '\r')
            au_buf_puts(b, "\\r");
        else if (*s == '\\')
            au_buf_puts(b, "\\\\");
        else
            au_buf_putc(b, *s);
    }
}

const char *au_prefs_user_agent(const AuPrefs *p)
{
    return p && p->user_agent && p->user_agent[0] ? p->user_agent : AU_DEFAULT_USER_AGENT;
}

void au_prefs_serialize(const AuPrefs *p, AuBuf *out)
{
    au_buf_puts(out, "# Aura preferences. One key=value per line.\n");
    AuBuf v;
    au_buf_init(&v);
    for (size_t i = 0; i < NKEYS; i++) {
        au_buf_clear(&v);
        au_prefs_get(p, keys[i].name, &v);
        au_buf_puts(out, keys[i].name);
        au_buf_putc(out, '=');
        put_escaped(out, au_buf_cstr(&v));
        au_buf_putc(out, '\n');
    }
    for (size_t i = 0; i < p->nextra; i++) {
        au_buf_puts(out, p->extra[i].key);
        au_buf_putc(out, '=');
        put_escaped(out, p->extra[i].value);
        au_buf_putc(out, '\n');
    }
    au_buf_free(&v);
}

static bool valid_key(const char *s, size_t n)
{
    if (n == 0 || n > 128)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(au_is_alpha(c) || au_is_digit(c) || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool au_prefs_parse(AuPrefs *p, const char *data, size_t len)
{
    bool ok = true;
    AuBuf key, val;
    au_buf_init(&key);
    au_buf_init(&val);
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
        if (memchr(line, 0, n)) {
            ok = false;
            continue;
        }
        size_t s = 0;
        while (s < n && (line[s] == ' ' || line[s] == '\t'))
            s++;
        if (s == n || line[s] == '#')
            continue;
        const char *eq = memchr(line + s, '=', n - s);
        if (!eq) {
            ok = false;
            continue;
        }
        size_t kn = (size_t)(eq - line) - s;
        while (kn && (line[s + kn - 1] == ' ' || line[s + kn - 1] == '\t'))
            kn--;
        if (!valid_key(line + s, kn)) {
            ok = false;
            continue;
        }
        au_buf_clear(&key);
        au_buf_append(&key, line + s, kn);
        au_buf_clear(&val);
        const char *v = eq + 1, *end = line + n;
        while (v < end && (*v == ' ' || *v == '\t'))
            v++; /* "key = value" */
        for (; v < end; v++) {
            if (*v == '\\' && v + 1 < end) {
                v++;
                au_buf_putc(&val, *v == 'n' ? '\n' : *v == 'r' ? '\r' : *v);
            } else {
                au_buf_putc(&val, *v);
            }
        }
        au_prefs_set(p, au_buf_cstr(&key), au_buf_cstr(&val));
    }
    au_buf_free(&key);
    au_buf_free(&val);
    /* A file saved by a version that stored its default string. */
    if (p->user_agent && (!strcmp(p->user_agent, AU_DEFAULT_USER_AGENT) ||
                          !strncmp(p->user_agent, AU_OLD_USER_AGENT_PREFIX, strlen(AU_OLD_USER_AGENT_PREFIX))))
        au_prefs_set_str(&p->user_agent, "");
    return ok;
}
