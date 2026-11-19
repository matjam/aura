/* The preference descriptions and the editing session (prefs_ui.h):
 * every setting the Preferences window shows is described; values round
 * trip; numbers, choices and the blocking fields are checked as the Mac
 * window checked them; enabling, notes, sections reset to defaults; live
 * items and Cancel; OK writes the preferences and marks them changed. */
#include "browser/browser.h"
#include "browser/credits.h"
#include "browser/prefs_ui.h"
#include "browser/scriptmem.h"
#include "core/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

static uint32_t t_ms(void *ctx) { (void)ctx; return 0; }
static AuTime t_now(void *ctx) { (void)ctx; return 1763100000; }
static bool t_read(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx, (void)url, (void)data, (void)len;
    return false;
}
static void t_inval(void *ctx, AuBrowserWindow *w, AuIRect r) { (void)ctx, (void)w, (void)r; }
static void t_changed(void *ctx, AuBrowserWindow *w, unsigned what) { (void)ctx, (void)w, (void)what; }
static void t_close(void *ctx, AuBrowserWindow *w) { (void)ctx, (void)w; }
static bool t_load(void *ctx, const void **data, size_t *len)
{
    (void)ctx, (void)data, (void)len;
    return false;
}

/* The text of a field. */
static const char *text_of(const AuPrefsEdit *e, AuPrefId id, AuBuf *b)
{
    au_buf_clear(b);
    au_prefs_edit_get_text(e, id, b);
    return au_buf_cstr(b);
}

static const char *note_of(const AuPrefsEdit *e, AuPrefId id, AuBuf *b)
{
    au_buf_clear(b);
    au_prefs_edit_text(e, id, b);
    return au_buf_cstr(b);
}

/* Every preference the Mac window showed before the descriptions. */
static const char *const shown_keys[] = {
    "home_page", "startup", "tabs.position", "appearance.color_scheme", "search.template", "links.external_new_window", "fonts.serif",
    "fonts.sans_serif", "fonts.monospace", "fonts.size", "fonts.size_monospace", "fonts.minimum_size",
    "content.javascript", "content.images", "content.block_popups", "content.script_memory_kb",
    "content.worker_memory_kb", "privacy.do_not_track", "privacy.global_privacy_control", "privacy.https_first",
    "security.legacy_tls", "privacy.warn_app_downloads", "privacy.cookies", "privacy.block_third_party_cookies",
    "history.enabled", "history.days", "privacy.clear_history_on_quit", "privacy.clear_cookies_on_quit",
    "privacy.clear_cache_on_quit", "proxy.http.host", "proxy.http.port", "proxy.https.host", "proxy.https.port",
    "network.identify", "network.os", "network.cpu", "network.user_agent", "cache.memory_kb", "cache.disk_kb", "tabs.unload_low_memory", "downloads.folder",
    "downloads.decode", "privacy.block_trackers", "blocking.lists", "blocking.update", "blocking.custom",
    "blocking.allow", "blocking.block",
};

static void test_descriptions(void)
{
    AuPrefs p;
    au_prefs_init(&p);
    AuBuf b;
    au_buf_init(&b);
    int last_section = 0;
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        const AuPrefDesc *d = au_pref_desc((AuPrefId)i);
        CHECK(d && d->id == i, "item %d is at its index", i);
        if (!d)
            continue;
        CHECK((int)d->section >= last_section, "item %d keeps the section order", i);
        last_section = d->section;
        CHECK(d->section < AU_PREF_SECTION_COUNT, "item %d has a section", i);
        if (d->key) {
            au_buf_clear(&b);
            CHECK(au_prefs_get(&p, d->key, &b), "item %d's key %s is a preference", i, d->key);
            CHECK(au_pref_desc_by_key(d->key) == d, "key %s finds item %d", d->key, i);
        }
        if (d->kind == AU_PREF_CHECK || d->kind == AU_PREF_ACTION)
            CHECK(d->title && *d->title, "item %d has a title", i);
        if (d->kind == AU_PREF_CHOICE || d->kind == AU_PREF_RADIO)
            CHECK(au_pref_choice_count(d) >= 2, "item %d has choices", i);
        if (d->kind == AU_PREF_NUMBER)
            CHECK(d->min < d->max, "item %d has a range", i);
        if (d->enabled_by != AU_PREF_NONE) {
            const AuPrefDesc *by = au_pref_desc(d->enabled_by);
            CHECK(by && (by->kind == AU_PREF_CHOICE || by->kind == AU_PREF_RADIO), "item %d's enabler is a choice",
                  i);
        }
    }
    CHECK(au_pref_desc(AU_PREF_COUNT) == NULL && au_pref_desc(AU_PREF_NONE) == NULL, "out of range");
    for (size_t k = 0; k < sizeof shown_keys / sizeof shown_keys[0]; k++)
        CHECK(au_pref_desc_by_key(shown_keys[k]) != NULL, "%s is described", shown_keys[k]);
    CHECK(au_pref_desc_by_key("zoom.sites") == NULL, "zoom.sites is not in the window");
    CHECK(!strcmp(au_pref_section_title(AU_PREF_SECTION_PRIVACY), "Privacy and Security"), "section title");

    /* A number's range is the preferences file's (au_prefs_set clamps the
     * same way). */
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        const AuPrefDesc *d = au_pref_desc((AuPrefId)i);
        if (d->kind != AU_PREF_NUMBER || !d->key)
            continue;
        AuPrefs q;
        au_prefs_init(&q);
        au_prefs_set(&q, d->key, "-99999999");
        au_buf_clear(&b);
        au_prefs_get(&q, d->key, &b);
        CHECK(atoi(au_buf_cstr(&b)) == d->min, "%s: low %s, window %d", d->key, au_buf_cstr(&b), d->min);
        au_prefs_set(&q, d->key, "99999999");
        au_buf_clear(&b);
        au_prefs_get(&q, d->key, &b);
        CHECK(atoi(au_buf_cstr(&b)) == d->max, "%s: high %s, window %d", d->key, au_buf_cstr(&b), d->max);
        au_prefs_free(&q);
    }
    au_buf_free(&b);
    au_prefs_free(&p);
}

static void test_round_trip(const AuPrefsHost *host)
{
    AuPrefs p;
    au_prefs_init(&p);
    AuPrefsEdit *e = au_prefs_edit_new(&p, host);
    AuBuf b;
    au_buf_init(&b);
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        const AuPrefDesc *d = au_pref_desc((AuPrefId)i);
        AuPrefId id = d->id;
        switch (d->kind) {
        case AU_PREF_CHECK: {
            bool v = au_prefs_edit_get_bool(e, id);
            CHECK(au_prefs_edit_set_bool(e, id, !v) && au_prefs_edit_get_bool(e, id) == !v, "%d: bool", i);
            au_prefs_edit_set_bool(e, id, v);
            CHECK(au_prefs_edit_get_bool(e, id) == v, "%d: bool back", i);
            break;
        }
        case AU_PREF_NUMBER: {
            CHECK(au_prefs_edit_set_int(e, id, d->min) && au_prefs_edit_get_int(e, id) == d->min, "%d: min", i);
            CHECK(au_prefs_edit_set_int(e, id, d->max) && au_prefs_edit_get_int(e, id) == d->max, "%d: max", i);
            char n[24];
            snprintf(n, sizeof n, "%d", d->min + 1);
            CHECK(au_prefs_edit_set_text(e, id, n) && !strcmp(text_of(e, id, &b), n), "%d: text %s", i, b.data);
            break;
        }
        case AU_PREF_TEXT:
        case AU_PREF_FOLDER:
            if (id == AU_PREF_BLOCK_CUSTOM) {
                CHECK(au_prefs_edit_set_text(e, id, "https://example.org/list.txt") &&
                          !strcmp(text_of(e, id, &b), "https://example.org/list.txt"),
                      "%d: text", i);
            } else if (id == AU_PREF_BLOCK_ALLOW || id == AU_PREF_BLOCK_DENY) {
                CHECK(au_prefs_edit_set_text(e, id, "example.org") && !strcmp(text_of(e, id, &b), "example.org"),
                      "%d: text", i);
            } else {
                CHECK(au_prefs_edit_set_text(e, id, "Macintosh HD:x y") &&
                          !strcmp(text_of(e, id, &b), "Macintosh HD:x y"),
                      "%d: text %s", i, b.data);
            }
            break;
        case AU_PREF_CHOICE:
        case AU_PREF_RADIO:
            for (size_t k = 0; k < au_prefs_edit_choice_count(e, id); k++) {
                CHECK(au_prefs_edit_set_choice(e, id, (int)k) && au_prefs_edit_get_choice(e, id) == (int)k,
                      "%d: choice %d", i, (int)k);
                CHECK(au_prefs_edit_choice(e, id, k) == d->choices[k], "%d: choice text", i);
            }
            break;
        case AU_PREF_FONT:
            CHECK(au_prefs_edit_choice_count(e, id) == 3, "%d: three families", i);
            CHECK(!strcmp(au_prefs_edit_choice(e, id, 0), "Charcoal") &&
                      !strcmp(au_prefs_edit_choice(e, id, 1), "Geneva") &&
                      !strcmp(au_prefs_edit_choice(e, id, 2), "Times"),
                  "%d: sorted and unique", i);
            CHECK(au_prefs_edit_set_choice(e, id, 1) && au_prefs_edit_get_choice(e, id) == 1 &&
                      !strcmp(text_of(e, id, &b), "Geneva"),
                  "%d: font by index", i);
            CHECK(au_prefs_edit_set_text(e, id, "times") && !strcmp(text_of(e, id, &b), "Times"),
                  "%d: font by name, any case: %s", i, b.data);
            CHECK(!au_prefs_edit_set_text(e, id, "Chicago") && !strcmp(text_of(e, id, &b), "Times"),
                  "%d: a family that is not installed is refused", i);
            CHECK(!au_prefs_edit_set_choice(e, id, 3) && !au_prefs_edit_set_choice(e, id, -1) &&
                      !strcmp(text_of(e, id, &b), "Times"),
                  "%d: an index out of range is refused", i);
            break;
        default:
            break;
        }
    }
    /* Getters of the wrong kind. */
    CHECK(!au_prefs_edit_set_bool(e, AU_PREF_HOME_PAGE, true), "a text item is not a bool");
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_JAVASCRIPT) == -1, "a check box has no choice");
    CHECK(!au_prefs_edit_set_text(e, AU_PREF_JAVASCRIPT, "1"), "a check box takes no text");
    /* A family of the preferences that is not installed shows no choice. */
    au_prefs_set_str(&p.font_serif, "New York");
    AuPrefsEdit *e2 = au_prefs_edit_new(&p, host);
    CHECK(au_prefs_edit_get_choice(e2, AU_PREF_FONT_SERIF) == -1, "an uninstalled family has no index");
    au_prefs_edit_free(e2);
    au_buf_free(&b);
    au_prefs_edit_free(e);
    au_prefs_free(&p);
}

/* The rules the Mac window applied on OK, at their boundaries. */
static void test_clamping(const AuPrefsHost *host)
{
    AuPrefs p, out;
    au_prefs_init(&p);
    AuPrefsEdit *e = au_prefs_edit_new(&p, host);
    AuBuf b;
    au_buf_init(&b);
    static const struct {
        AuPrefId id;
        const char *text;
        int want;
        bool ok;
    } cases[] = {
        {AU_PREF_FONT_SIZE, "5", 6, true},
        {AU_PREF_FONT_SIZE, "6", 6, true},
        {AU_PREF_FONT_SIZE, "72", 72, true},
        {AU_PREF_FONT_SIZE, "73", 72, true},
        {AU_PREF_FONT_SIZE, "-1", 6, true},
        {AU_PREF_FONT_SIZE, "99999999999999999999", 72, true},
        {AU_PREF_FONT_SIZE, "abc", 16, false}, /* the value when the window opened */
        {AU_PREF_FONT_SIZE, "", 16, false},
        {AU_PREF_FONT_SIZE, "12px", 16, false},
        {AU_PREF_FONT_SIZE, " 14", 14, true}, /* strtol skips leading space, as before */
        {AU_PREF_FONT_SIZE_MONO, "5", 6, true},
        {AU_PREF_FONT_SIZE_MONO, "73", 72, true},
        {AU_PREF_FONT_SIZE_MIN, "-5", 0, true},
        {AU_PREF_FONT_SIZE_MIN, "0", 0, true},
        {AU_PREF_FONT_SIZE_MIN, "36", 36, true},
        {AU_PREF_FONT_SIZE_MIN, "37", 36, true},
        {AU_PREF_HISTORY_DAYS, "3650", 3650, true},
        {AU_PREF_HISTORY_DAYS, "3651", 3650, true},
        {AU_PREF_HISTORY_DAYS, "-1", 0, true},
        {AU_PREF_HTTP_PROXY_PORT, "65535", 65535, true},
        {AU_PREF_HTTP_PROXY_PORT, "65536", 65535, true},
        {AU_PREF_HTTP_PROXY_PORT, "-1", 0, true},
        {AU_PREF_HTTP_PROXY_PORT, "x", 8080, false},
        {AU_PREF_HTTPS_PROXY_PORT, "65536", 65535, true},
        {AU_PREF_MEMORY_CACHE, "1048576", 1048576, true},
        {AU_PREF_MEMORY_CACHE, "1048577", 1048576, true},
        {AU_PREF_DISK_CACHE, "4194304", 4194304, true},
        {AU_PREF_DISK_CACHE, "4194305", 4194304, true},
        {AU_PREF_DISK_CACHE, "-3", 0, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        bool ok = au_prefs_edit_set_text(e, cases[i].id, cases[i].text);
        CHECK(ok == cases[i].ok && au_prefs_edit_get_int(e, cases[i].id) == cases[i].want,
              "item %d \"%s\": %d (%d), want %d (%d)", cases[i].id, cases[i].text, au_prefs_edit_get_int(e, cases[i].id),
              ok, cases[i].want, cases[i].ok);
    }

    /* Choices: clamped, except the update interval, where anything out of
     * range is daily. */
    au_prefs_edit_set_choice(e, AU_PREF_STARTUP, 5);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_STARTUP) == AU_STARTUP_RESTORE, "startup clamps high");
    au_prefs_edit_set_choice(e, AU_PREF_STARTUP, -1);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_STARTUP) == AU_STARTUP_HOME, "startup clamps low");
    au_prefs_edit_set_choice(e, AU_PREF_COOKIES, 3);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_COOKIES) == AU_COOKIES_NONE, "cookies clamp");
    au_prefs_edit_set_choice(e, AU_PREF_TAB_POSITION, 2);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_TAB_POSITION) == AU_TABS_LEFT, "tabs clamp");
    au_prefs_edit_set_choice(e, AU_PREF_BLOCK_UPDATE, 2);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_BLOCK_UPDATE) == AU_BLOCK_UPDATE_NEVER, "never");
    au_prefs_edit_set_choice(e, AU_PREF_BLOCK_UPDATE, 3);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_BLOCK_UPDATE) == AU_BLOCK_UPDATE_DAILY, "out of range is daily");
    au_prefs_edit_set_choice(e, AU_PREF_BLOCK_UPDATE, -1);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_BLOCK_UPDATE) == AU_BLOCK_UPDATE_DAILY, "below range is daily");

    /* The user agent: Identify as Aura by default, which names no other
     * browser; the field shows the string sent and is editable only for
     * Custom. */
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_IDENTIFY) == AU_IDENTIFY_AURA, "Aura by default");
    CHECK(!strcmp(text_of(e, AU_PREF_USER_AGENT, &b), AU_DEFAULT_USER_AGENT), "the default user agent shows");
    CHECK(!strstr(AU_DEFAULT_USER_AGENT, "Chrome") && !strstr(AU_DEFAULT_USER_AGENT, "Safari") &&
              strstr(AU_DEFAULT_USER_AGENT, " Aura/"),
          "Aura's user agent claims another browser: %s", AU_DEFAULT_USER_AGENT);
    CHECK(!au_prefs_edit_enabled(e, AU_PREF_USER_AGENT), "the field is editable without Custom");
    au_prefs_edit_set_text(e, AU_PREF_USER_AGENT, "Mozilla/1.0");
    CHECK(!strcmp(au_prefs_edit_prefs(e)->user_agent, ""), "text kept without Custom");
    au_prefs_edit_set_choice(e, AU_PREF_IDENTIFY, AU_IDENTIFY_CHROME);
    text_of(e, AU_PREF_USER_AGENT, &b);
    CHECK(strstr(au_buf_cstr(&b), " Chrome/") && !strstr(au_buf_cstr(&b), "Aura") && !strstr(au_buf_cstr(&b), "PPC"),
          "Chrome's string: %s", au_buf_cstr(&b));
    au_prefs_edit_set_choice(e, AU_PREF_IDENTIFY, AU_IDENTIFY_SAFARI);
    text_of(e, AU_PREF_USER_AGENT, &b);
    CHECK(strstr(au_buf_cstr(&b), " Version/") && strstr(au_buf_cstr(&b), "Mac OS X"), "Safari's string: %s",
          au_buf_cstr(&b));
    au_prefs_edit_set_choice(e, AU_PREF_UA_OS, AU_UAOS_WINDOWS);
    au_prefs_edit_set_choice(e, AU_PREF_UA_CPU, AU_UACPU_X86_64);
    au_prefs_edit_set_choice(e, AU_PREF_IDENTIFY, AU_IDENTIFY_FIREFOX);
    CHECK(!strcmp(text_of(e, AU_PREF_USER_AGENT, &b), "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:" AU_UA_FIREFOX_VERSION
                                                      ") Gecko/20100101 Firefox/" AU_UA_FIREFOX_VERSION),
          "Firefox on Windows: %s", au_buf_cstr(&b));
    au_prefs_edit_set_choice(e, AU_PREF_UA_OS, AU_UAOS_THIS);
    au_prefs_edit_set_choice(e, AU_PREF_UA_CPU, AU_UACPU_THIS);
    au_prefs_edit_set_choice(e, AU_PREF_IDENTIFY, AU_IDENTIFY_CUSTOM);
    CHECK(au_prefs_edit_enabled(e, AU_PREF_USER_AGENT), "the field is not editable for Custom");
    au_prefs_edit_set_text(e, AU_PREF_USER_AGENT, "Mozilla/1.0");
    CHECK(!strcmp(au_prefs_edit_prefs(e)->user_agent, "Mozilla/1.0"), "a custom user agent is kept");
    CHECK(!strcmp(text_of(e, AU_PREF_USER_AGENT, &b), "Mozilla/1.0"), "the custom user agent shows");
    au_prefs_edit_set_choice(e, AU_PREF_IDENTIFY, AU_IDENTIFY_AURA);
    CHECK(!strcmp(text_of(e, AU_PREF_USER_AGENT, &b), AU_DEFAULT_USER_AGENT), "back to Aura's own");
    CHECK(!strcmp(au_prefs_edit_prefs(e)->user_agent, "Mozilla/1.0"), "the custom string is kept for later");
    au_prefs_edit_set_choice(e, AU_PREF_IDENTIFY, AU_IDENTIFY_CUSTOM);
    au_prefs_edit_set_text(e, AU_PREF_USER_AGENT, "");
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_IDENTIFY) == AU_IDENTIFY_AURA, "an empty custom string is Aura");
    CHECK(!strcmp(text_of(e, AU_PREF_USER_AGENT, &b), AU_DEFAULT_USER_AGENT), "cleared is the default");

    /* The blocking fields. */
    au_prefs_edit_set_text(e, AU_PREF_BLOCK_CUSTOM,
                           "http://a.com/l.txt https://x.y  https://ab.cd\thttps://a.b/c.txt\nHTTPS://EXAMPLE.ORG/l ftp:x");
    CHECK(!strcmp(text_of(e, AU_PREF_BLOCK_CUSTOM, &b), "https://ab.cd https://a.b/c.txt HTTPS://EXAMPLE.ORG/l"),
          "only https addresses of 12 bytes or more: %s", b.data);
    {
        AuBuf long_url;
        au_buf_init(&long_url);
        au_buf_puts(&long_url, "https://");
        while (long_url.len < 1023)
            au_buf_putc(&long_url, 'a');
        au_prefs_edit_set_text(e, AU_PREF_BLOCK_CUSTOM, au_buf_cstr(&long_url));
        CHECK(!strcmp(text_of(e, AU_PREF_BLOCK_CUSTOM, &b), au_buf_cstr(&long_url)), "1023 bytes is kept");
        au_buf_putc(&long_url, 'a');
        au_prefs_edit_set_text(e, AU_PREF_BLOCK_CUSTOM, au_buf_cstr(&long_url));
        CHECK(!strcmp(text_of(e, AU_PREF_BLOCK_CUSTOM, &b), ""), "1024 bytes is dropped");
        au_buf_free(&long_url);
    }
    au_prefs_edit_set_text(e, AU_PREF_BLOCK_ALLOW, "Example.COM, example.com,foo.org  https://bar.net/x");
    CHECK(!strcmp(text_of(e, AU_PREF_BLOCK_ALLOW, &b), "example.com foo.org bar.net"),
          "domains cleaned, commas separate, no repeats: %s", b.data);
    au_prefs_edit_set_text(e, AU_PREF_BLOCK_DENY, ",,, ");
    CHECK(!strcmp(text_of(e, AU_PREF_BLOCK_DENY, &b), ""), "nothing");

    /* Script memory: Automatic is 0; Custom is 4 to 256 MB; text that is
     * not a number takes the value from when the window opened (0 for
     * Automatic, so the floor). */
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_SCRIPT_MEMORY_MODE) == 0, "Automatic");
    au_prefs_init(&out);
    au_prefs_edit_apply(e, &out);
    CHECK(out.script_memory_kb == 0 && out.worker_memory_kb == 0, "Automatic is stored as 0");
    CHECK(out.image_memory_kb == 0, "images: Automatic is stored as 0");
    au_prefs_edit_set_choice(e, AU_PREF_SCRIPT_MEMORY_MODE, 1);
    au_prefs_edit_set_choice(e, AU_PREF_WORKER_MEMORY_MODE, 1);
    au_prefs_edit_set_choice(e, AU_PREF_IMAGE_MEMORY_MODE, 1);
    au_prefs_edit_set_text(e, AU_PREF_IMAGE_MEMORY_MB, "48");
    au_prefs_edit_apply(e, &out);
    CHECK(out.image_memory_kb == 48 * 1024, "images: Custom 48 MB, got %d", out.image_memory_kb);
    au_prefs_edit_set_text(e, AU_PREF_IMAGE_MEMORY_MB, "1");
    au_prefs_edit_apply(e, &out);
    CHECK(out.image_memory_kb == 4096, "images: the floor, got %d", out.image_memory_kb);
    /* Automatic: an eighth of what is free, 8 to 64 MB (times four with
     * 64-bit pointers) */
    {
        uint32_t s = sizeof(void *) >= 8 ? 4 : 1;
        CHECK(au_image_memory_auto(0) == 8192 * s, "images unknown: %u", au_image_memory_auto(0));
        CHECK(au_image_memory_auto(40 * 1024) == 8192 * s, "40 MB free");
        CHECK(au_image_memory_auto(330 * 1024) == 41 * 1024 * s, "330 MB free: %u",
              au_image_memory_auto(330 * 1024));
        CHECK(au_image_memory_auto(4096 * 1024) == 64 * 1024 * s, "4 GB free");
        CHECK(au_image_memory_limit(20480, 330 * 1024) == 20480, "custom wins");
    }
    static const struct {
        const char *text;
        int kb;
    } mem[] = {{"3", 4096}, {"4", 4096}, {"256", 262144}, {"257", 262144}, {"0", 4096},
               {"-7", 4096}, {"5000", 262144}, {"abc", 4096}, {"40", 40960}};
    for (size_t i = 0; i < sizeof mem / sizeof mem[0]; i++) {
        au_prefs_edit_set_text(e, AU_PREF_SCRIPT_MEMORY_MB, mem[i].text);
        au_prefs_edit_set_text(e, AU_PREF_WORKER_MEMORY_MB, mem[i].text);
        au_prefs_edit_apply(e, &out);
        CHECK(out.script_memory_kb == mem[i].kb && out.worker_memory_kb == mem[i].kb, "\"%s\": %d %d, want %d",
              mem[i].text, out.script_memory_kb, out.worker_memory_kb, mem[i].kb);
    }
    au_prefs_free(&out);
    au_prefs_edit_free(e);

    /* A custom limit when the window opened is the fallback, and shows. */
    p.script_memory_kb = 8192;
    e = au_prefs_edit_new(&p, host);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_SCRIPT_MEMORY_MODE) == 1 &&
              au_prefs_edit_get_int(e, AU_PREF_SCRIPT_MEMORY_MB) == 8,
          "Custom 8 MB loads");
    au_prefs_edit_set_text(e, AU_PREF_SCRIPT_MEMORY_MB, "12");
    au_prefs_edit_set_text(e, AU_PREF_SCRIPT_MEMORY_MB, "x");
    CHECK(au_prefs_edit_get_int(e, AU_PREF_SCRIPT_MEMORY_MB) == 8, "not a number: back to 8");
    au_buf_free(&b);
    au_prefs_edit_free(e);
    au_prefs_free(&p);
}

static void test_enabled_and_notes(const AuPrefsHost *host)
{
    AuPrefs p;
    au_prefs_init(&p);
    AuPrefsHost h = *host;
    h.memory_free_kb = 0; /* unknown: 64 MB and 32 MB, twice that with 64-bit pointers */
    AuPrefsEdit *e = au_prefs_edit_new(&p, &h);
    AuBuf b;
    au_buf_init(&b);
    CHECK(!au_prefs_edit_enabled(e, AU_PREF_SCRIPT_MEMORY_MB) && !au_prefs_edit_enabled(e, AU_PREF_WORKER_MEMORY_MB),
          "the MB fields are off for Automatic");
    CHECK(au_prefs_edit_get_int(e, AU_PREF_SCRIPT_MEMORY_MB) == 64 * (int)AU_SCRIPT_MEM_SCALE &&
              au_prefs_edit_get_int(e, AU_PREF_WORKER_MEMORY_MB) == 32 * (int)AU_SCRIPT_MEM_SCALE,
          "Automatic shows what it gives");
    au_prefs_edit_set_choice(e, AU_PREF_SCRIPT_MEMORY_MODE, 1);
    CHECK(au_prefs_edit_enabled(e, AU_PREF_SCRIPT_MEMORY_MB) && !au_prefs_edit_enabled(e, AU_PREF_WORKER_MEMORY_MB),
          "Custom turns its own field on");
    CHECK(!strcmp(note_of(e, AU_PREF_SCRIPT_MEMORY_MB, &b),
                  AU_SCRIPT_MEM_SCALE == 2 ? "Automatic is 128 MB on this Mac" : "Automatic is 64 MB on this Mac"),
          "note: %s", b.data);
    au_prefs_edit_set_memory_free(e, 40u * 1024);
    CHECK(!strcmp(note_of(e, AU_PREF_WORKER_MEMORY_MB, &b), "Automatic is 10 MB on this Mac"), "note: %s", b.data);
    CHECK(strstr(note_of(e, AU_PREF_SCRIPT_MEMORY_NOTE, &b), "memory free on this Mac") &&
              strstr(b.data, "4 to 256 MB"),
          "paragraph: %s", b.data);
    CHECK(au_prefs_edit_enabled(e, AU_PREF_HOME_PAGE), "other items are on");

    CHECK(au_prefs_edit_enabled(e, AU_PREF_DEFAULT_BROWSER) && !strcmp(note_of(e, AU_PREF_DEFAULT_BROWSER, &b), ""),
          "not the default: no text");
    au_prefs_edit_set_default_browser(e, AU_DEFAULT_BROWSER_YES);
    CHECK(!strcmp(note_of(e, AU_PREF_DEFAULT_BROWSER, &b), "Aura is the default."), "the default");
    au_prefs_edit_set_default_browser(e, AU_DEFAULT_BROWSER_UNAVAILABLE);
    CHECK(!au_prefs_edit_enabled(e, AU_PREF_DEFAULT_BROWSER) &&
              !strcmp(note_of(e, AU_PREF_DEFAULT_BROWSER, &b), "Internet Config is not available."),
          "unavailable: %s", b.data);

    CHECK(!strcmp(note_of(e, AU_PREF_DOWNLOAD_FOLDER_NOTE, &b),
                  "A folder path such as Macintosh HD:Downloads. Leave it empty for Internet Config's download folder "
                  "(Downloads)."),
          "download note: %s", b.data);
    CHECK(!strcmp(note_of(e, AU_PREF_SEARCH_NOTE, &b),
                  "Text in the address field that is not a web address is searched for with this URL; %s stands for "
                  "the words."),
          "search note");
    CHECK(!strcmp(note_of(e, AU_PREF_BLOCK_CUSTOM, &b), ""), "no manager: no status");
    CHECK(!strcmp(note_of(e, AU_PREF_HOME_PAGE, &b), ""), "no note");
    au_prefs_edit_free(e);

    h.system_downloads = NULL;
    e = au_prefs_edit_new(&p, &h);
    CHECK(!strcmp(note_of(e, AU_PREF_DOWNLOAD_FOLDER_NOTE, &b),
                  "A folder path such as Macintosh HD:Downloads. Leave it empty for the desktop."),
          "download note without a system folder: %s", b.data);
    au_prefs_edit_free(e);
    au_buf_free(&b);
    au_prefs_free(&p);
}

static void test_reset(const AuPrefsHost *host)
{
    AuPrefs p;
    au_prefs_init(&p);
    p.font_size = 20;
    p.font_size_min = 9;
    au_prefs_set_str(&p.font_serif, "Geneva");
    p.javascript = false;
    p.script_memory_kb = 8192;
    p.send_dnt = true;
    AuPrefsEdit *e = au_prefs_edit_new(&p, host);
    au_prefs_edit_reset_section(e, AU_PREF_SECTION_FONTS);
    const AuPrefs *w = au_prefs_edit_prefs(e);
    CHECK(w->font_size == 16 && w->font_size_min == 0 && !strcmp(w->font_serif, "Times New Roman"),
          "fonts are back to their defaults");
    CHECK(!w->javascript && w->send_dnt, "other sections are not");
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_SCRIPT_MEMORY_MODE) == 1, "script memory is not");
    au_prefs_edit_reset_section(e, AU_PREF_SECTION_CONTENT);
    CHECK(w->javascript && au_prefs_edit_get_choice(e, AU_PREF_SCRIPT_MEMORY_MODE) == 0, "content is back");
    au_prefs_edit_set_block_list(e, 0, false);
    au_prefs_edit_set_text(e, AU_PREF_BLOCK_ALLOW, "a.com");
    au_prefs_edit_reset_section(e, AU_PREF_SECTION_BLOCKING);
    CHECK(!strcmp(w->block_lists, "light") && !strcmp(w->block_allow, ""), "blocking is back");
    CHECK(w->send_dnt, "privacy is untouched");
    au_prefs_edit_free(e);
    au_prefs_free(&p);
}

static void test_apply_and_live(const AuPrefsHost *host, AuFontSet *fonts)
{
    AuBrowserHost bh;
    memset(&bh, 0, sizeof bh);
    bh.time_ms = t_ms;
    bh.now = t_now;
    bh.read_file_url = t_read;
    bh.invalidate = t_inval;
    bh.changed = t_changed;
    bh.close_window = t_close;
    AuBrowser *b = au_browser_new(fonts, &bh);
    AuPrefsEdit *e = au_prefs_edit_new(&b->prefs, host);

    /* Nothing changed: applying changes nothing. */
    AuPrefs copy;
    au_prefs_copy(&copy, &b->prefs);
    CHECK(!au_prefs_edit_apply(e, &copy), "nothing to apply");
    au_prefs_free(&copy);

    /* Live: the tab position and the blocking lists apply at once. */
    uint32_t gen = b->prefs_gen;
    au_prefs_edit_set_choice(e, AU_PREF_TAB_POSITION, AU_TABS_LEFT);
    CHECK(au_prefs_edit_apply_live(e, b) == AU_PREF_CHANGED_TABS && b->prefs.tab_position == AU_TABS_LEFT,
          "the tab position applies at once");
    CHECK(b->prefs_gen != gen, "and marks the preferences changed");
    CHECK(au_prefs_edit_apply_live(e, b) == 0, "applying again changes nothing");
    CHECK(au_prefs_edit_block_list_count(e) == 7, "seven catalogue lists");
    AuPrefBlockRow row;
    CHECK(au_prefs_edit_block_list(e, 0, &row) && !strcmp(row.id, "light") && row.on && !strcmp(row.status, ""),
          "the light list is on");
    CHECK(au_prefs_edit_block_list(e, 1, &row) && !row.on, "the normal list is off");
    CHECK(!au_prefs_edit_block_list(e, 7, &row), "past the end");
    au_prefs_edit_set_block_list(e, 2, true);
    au_prefs_edit_set_block_list(e, 1, true);
    CHECK(!strcmp(au_prefs_edit_prefs(e)->block_lists, "light normal pro"), "catalogue order: %s",
          au_prefs_edit_prefs(e)->block_lists);
    gen = b->prefs_gen;
    CHECK(au_prefs_edit_apply_live(e, b) == AU_PREF_CHANGED_BLOCKING &&
              !strcmp(b->prefs.block_lists, "light normal pro") && b->prefs_gen == gen + 1,
          "the lists apply at once");
    au_prefs_edit_set_bool(e, AU_PREF_BLOCK_TRACKERS, false);
    CHECK(au_prefs_edit_apply_live(e, b) == AU_PREF_CHANGED_BLOCKING && !b->prefs.block_trackers, "the switch too");

    /* The appearance is live too: every page takes the new scheme. */
    CHECK(au_pref_desc(AU_PREF_APPEARANCE)->flags & AU_PREF_LIVE, "the appearance is live");
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_APPEARANCE) == AU_SCHEME_SYSTEM, "the appearance starts at System");
    au_prefs_edit_set_choice(e, AU_PREF_APPEARANCE, AU_SCHEME_DARK);
    gen = b->prefs_gen;
    CHECK(au_prefs_edit_apply_live(e, b) == AU_PREF_CHANGED_APPEARANCE && b->prefs.color_scheme == AU_SCHEME_DARK &&
              au_browser_dark(b) && b->prefs_gen != gen,
          "the appearance applies at once");
    au_prefs_edit_set_choice(e, AU_PREF_APPEARANCE, 3);
    CHECK(au_prefs_edit_get_choice(e, AU_PREF_APPEARANCE) == AU_SCHEME_DARK, "appearance clamps");

    /* A setting that is not live waits for OK. */
    au_prefs_edit_set_bool(e, AU_PREF_JAVASCRIPT, false);
    au_prefs_edit_apply_live(e, b);
    CHECK(b->prefs.javascript, "JavaScript waits for OK");

    /* Cancel puts the live ones back. */
    unsigned bits = au_prefs_edit_cancel(e, b);
    CHECK(bits == (AU_PREF_CHANGED_TABS | AU_PREF_CHANGED_BLOCKING | AU_PREF_CHANGED_APPEARANCE), "cancel: %u", bits);
    CHECK(b->prefs.color_scheme == AU_SCHEME_SYSTEM && !au_browser_dark(b), "cancel restores the appearance");
    CHECK(b->prefs.tab_position == AU_TABS_TOP && !strcmp(b->prefs.block_lists, "light") && b->prefs.block_trackers &&
              b->prefs.javascript,
          "cancel restores");
    au_prefs_edit_free(e);

    /* OK: every shown setting is written, the rest is left. */
    e = au_prefs_edit_new(&b->prefs, host);
    au_prefs_set_str(&b->prefs.zoom_sites, "example.com=150"); /* changed elsewhere while the window is open */
    au_prefs_edit_set_text(e, AU_PREF_HOME_PAGE, "https://example.org/");
    au_prefs_edit_set_text(e, AU_PREF_FONT_SIZE, "18");
    au_prefs_edit_set_text(e, AU_PREF_FONT_SANS, "Geneva");
    au_prefs_edit_set_bool(e, AU_PREF_SEND_DNT, true);
    au_prefs_edit_set_choice(e, AU_PREF_COOKIES, AU_COOKIES_SAME_SITE);
    au_prefs_edit_set_choice(e, AU_PREF_WORKER_MEMORY_MODE, 1);
    au_prefs_edit_set_text(e, AU_PREF_WORKER_MEMORY_MB, "16");
    au_prefs_edit_set_text(e, AU_PREF_DOWNLOAD_FOLDER, "Macintosh HD:Downloads");
    au_prefs_edit_set_choice(e, AU_PREF_BLOCK_UPDATE, AU_BLOCK_UPDATE_WEEKLY);
    gen = b->prefs_gen;
    CHECK(au_prefs_edit_commit(e, b), "OK changed something");
    CHECK(b->prefs_gen != gen, "OK marks the preferences changed");
    CHECK(!strcmp(b->prefs.home_page, "https://example.org/") && b->prefs.font_size == 18 &&
              !strcmp(b->prefs.font_sans, "Geneva") && b->prefs.send_dnt && b->prefs.cookies == AU_COOKIES_SAME_SITE &&
              b->prefs.worker_memory_kb == 16384 && b->prefs.script_memory_kb == 0 &&
              !strcmp(b->prefs.download_folder, "Macintosh HD:Downloads") &&
              b->prefs.block_update == AU_BLOCK_UPDATE_WEEKLY,
          "OK writes the values");
    CHECK(!strcmp(b->prefs.zoom_sites, "example.com=150"), "OK leaves settings the window does not show");
    au_prefs_edit_free(e);

    /* Buttons. */
    e = au_prefs_edit_new(&b->prefs, host);
    AuPrefId changed;
    CHECK(au_prefs_edit_action(e, AU_PREF_USE_CURRENT_PAGE, b, NULL, &changed) == 0 && changed == AU_PREF_NONE,
          "no window: nothing");
    CHECK(au_prefs_edit_action(e, AU_PREF_CLEAR_COOKIES, b, NULL, &changed) == AU_PREF_DO_CLEAR_COOKIES, "cookies");
    CHECK(au_prefs_edit_action(e, AU_PREF_CLEAR_CACHE, b, NULL, &changed) == AU_PREF_DO_CLEAR_CACHE, "cache");
    CHECK(au_prefs_edit_action(e, AU_PREF_SHOW_COOKIES, b, NULL, &changed) == AU_PREF_DO_SHOW_COOKIES, "show");
    CHECK(au_prefs_edit_action(e, AU_PREF_DEFAULT_BROWSER, b, NULL, &changed) == AU_PREF_DO_MAKE_DEFAULT, "default");
    CHECK(au_prefs_edit_action(e, AU_PREF_BLOCK_UPDATE_NOW, b, NULL, &changed) == AU_PREF_DO_REFRESH_LISTS, "update");
    CHECK(au_prefs_edit_action(e, AU_PREF_CLEAR_HISTORY, b, NULL, &changed) == 0, "history is the model's");
    CHECK(au_prefs_edit_action(e, AU_PREF_JAVASCRIPT, b, NULL, &changed) == 0, "not a button");
    au_prefs_edit_free(e);
    au_browser_free(b);
}

static void test_block_status(void)
{
    AuBlockListInfo li;
    char s[256];
    memset(&li, 0, sizeof li);
    li.entries = 223000;
    au_prefs_block_status(&li, s, sizeof s);
    CHECK(!strcmp(s, "about 223,000 domains, 1307 KB"), "estimate: %s", s);
    li.state = AU_BLOCK_WAITING;
    au_prefs_block_status(&li, s, sizeof s);
    CHECK(!strcmp(s, "waiting to download"), "waiting: %s", s);
    li.state = AU_BLOCK_DOWNLOADING;
    li.received = 5000;
    au_prefs_block_status(&li, s, sizeof s);
    CHECK(!strcmp(s, "downloading, 4 KB so far"), "downloading: %s", s);
    li.state = AU_BLOCK_COMPILING;
    au_prefs_block_status(&li, s, sizeof s);
    CHECK(!strcmp(s, "compiling"), "compiling: %s", s);
    li.state = AU_BLOCK_IDLE;
    snprintf(li.error, sizeof li.error, "HTTP 404");
    au_prefs_block_status(&li, s, sizeof s);
    CHECK(!strcmp(s, "not loaded: HTTP 404"), "error: %s", s);
    li.loaded = true;
    li.entries = 92184;
    li.bytes = 552960;
    li.updated_at = 1763251200; /* 16 Nov 2025 */
    au_prefs_block_status(&li, s, sizeof s);
    CHECK(!strcmp(s, "92,184 domains, 540 KB, 16 Nov; HTTP 404"), "loaded with an error: %s", s);
    li.error[0] = 0;
    li.updated_at = 0;
    au_prefs_block_status(&li, s, sizeof s);
    CHECK(!strcmp(s, "92,184 domains, 540 KB"), "loaded, no date: %s", s);
}

int main(void)
{
    AuFontSet *fonts = au_fontset_new();
    au_fontset_add_lazy(fonts, "Times", 400, false, 0, 0, t_load, NULL);
    au_fontset_add_lazy(fonts, "Geneva", 400, false, 0, 0, t_load, NULL);
    au_fontset_add_lazy(fonts, "Geneva", 700, false, 0, 0, t_load, NULL);
    au_fontset_add_lazy(fonts, "Charcoal", 400, false, 0, 0, t_load, NULL);
    AuPrefsHost host;
    memset(&host, 0, sizeof host);
    host.fonts = fonts;
    host.memory_free_kb = 0;
    host.default_browser = AU_DEFAULT_BROWSER_NO;
    host.machine = "this Mac";
    host.settings_name = "Internet Config";
    host.folder_example = "Macintosh HD:Downloads";
    host.system_downloads = "Downloads";
    host.no_folder = "the desktop";

    test_descriptions();
    test_round_trip(&host);
    test_clamping(&host);
    test_enabled_and_notes(&host);
    test_reset(&host);
    test_apply_and_live(&host, fonts);
    test_block_status();

    au_fontset_free(fonts);
    printf("prefs_ui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
