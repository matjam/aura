/* The Preferences window's contents and its editing session. See
 * prefs_ui.h. */
#include "prefs_ui.h"

#include "credits.h"
#include "favicon.h"
#include "history.h"
#include "page.h"
#include "scriptmem.h"
#include "../net/blocklist.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- The descriptions ---- */

static const char *const startup_choices[] = {"Open the home page", "Open a blank page", "Restore the last tabs", NULL};
static const char *const tab_choices[] = {"Along the top", "Down the left side", NULL};
static const char *const appearance_choices[] = {"Same as system", "Light", "Dark", NULL};
static const char *const image_colors_choices[] = {"Same as the screen", "Millions", "Thousands", "256 (dithered)",
                                                    NULL};
/* Indexes are AuPrefsEdit.mem_mode: 0 Automatic, 1 Custom, 2 Unlimited. */
static const char *const memory_choices[] = {"Automatic", "Custom", "Unlimited", NULL};
static const char *const cookie_choices[] = {"Accept all", "Same-site only", "Block all", NULL};
static const char *const update_choices[] = {"Every day", "Every week", "Never", NULL};
static const char *const identify_choices[] = {"Aura", "Chrome", "Safari", "Firefox", "Custom", NULL};
static const char *const ua_os_choices[] = {"This computer", "Mac OS 9", "Mac OS X 10.5", "macOS", "Windows", "Linux",
                                            NULL};
static const char *const ua_cpu_choices[] = {"This computer", "PowerPC", "x86-64 (Intel)", "ARM", NULL};

#define OFF(f) offsetof(AuPrefs, f)
#define SEC(s) AU_PREF_SECTION_##s

/* id, section, kind, key, caption, title, unit, help, choices, empty, min, max, enabled_by, enabled_value,
 * flags, store, offset */
#define D_CHECK(id, s, key, title, f) \
    {AU_PREF_##id, SEC(s), AU_PREF_CHECK, key, NULL, title, NULL, NULL, NULL, NULL, 0, 0, AU_PREF_NONE, 0, 0, \
     AU_PREF_STORE_BOOL, OFF(f)}
#define D_CHECK_CAP(id, s, key, caption, title, f) \
    {AU_PREF_##id, SEC(s), AU_PREF_CHECK, key, caption, title, NULL, NULL, NULL, NULL, 0, 0, AU_PREF_NONE, 0, 0, \
     AU_PREF_STORE_BOOL, OFF(f)}
#define D_TEXT(id, s, key, caption, f) \
    {AU_PREF_##id, SEC(s), AU_PREF_TEXT, key, caption, NULL, NULL, NULL, NULL, NULL, 0, 0, AU_PREF_NONE, 0, 0, \
     AU_PREF_STORE_STR, OFF(f)}
#define D_NUMBER(id, s, key, caption, unit, lo, hi, f) \
    {AU_PREF_##id, SEC(s), AU_PREF_NUMBER, key, caption, NULL, unit, NULL, NULL, NULL, lo, hi, AU_PREF_NONE, 0, 0, \
     AU_PREF_STORE_INT, OFF(f)}
#define D_CHOICE(id, s, kind, key, caption, choices, flags, f) \
    {AU_PREF_##id, SEC(s), kind, key, caption, NULL, NULL, NULL, choices, NULL, 0, 0, AU_PREF_NONE, 0, flags, \
     AU_PREF_STORE_ENUM, OFF(f)}
#define D_FONT(id, key, caption, f) \
    {AU_PREF_##id, SEC(FONTS), AU_PREF_FONT, key, caption, NULL, NULL, NULL, NULL, "(no fonts)", 0, 0, \
     AU_PREF_NONE, 0, 0, AU_PREF_STORE_STR, OFF(f)}
#define D_ACTION(id, s, title, flags) \
    {AU_PREF_##id, SEC(s), AU_PREF_ACTION, NULL, NULL, title, NULL, NULL, NULL, NULL, 0, 0, AU_PREF_NONE, 0, flags, \
     AU_PREF_STORE_NONE, 0}
#define D_NOTE(id, s, help) \
    {AU_PREF_##id, SEC(s), AU_PREF_NOTE, NULL, NULL, NULL, NULL, help, NULL, NULL, 0, 0, AU_PREF_NONE, 0, 0, \
     AU_PREF_STORE_NONE, 0}

static const AuPrefDesc descs[AU_PREF_COUNT] = {
    /* General */
    D_TEXT(HOME_PAGE, GENERAL, "home_page", "Home page:", home_page),
    D_ACTION(USE_CURRENT_PAGE, GENERAL, "Use Current Page", 0),
    D_CHOICE(STARTUP, GENERAL, AU_PREF_CHOICE, "startup", "When Aura starts:", startup_choices, 0, startup),
    D_CHOICE(TAB_POSITION, GENERAL, AU_PREF_RADIO, "tabs.position", "Tabs:", tab_choices, AU_PREF_LIVE, tab_position),
    D_CHOICE(APPEARANCE, GENERAL, AU_PREF_CHOICE, "appearance.color_scheme", "Appearance:", appearance_choices,
             AU_PREF_LIVE, color_scheme),
    D_TEXT(SEARCH, GENERAL, "search.template", "Search with:", search_template),
    D_NOTE(SEARCH_NOTE, GENERAL,
         "Text in the address field that is not a web address is searched for with this URL; %s stands for the "
         "words."),
    D_CHECK_CAP(EXTERNAL_NEW_WINDOW, GENERAL, "links.external_new_window", "Links from other applications:",
              "Open in a new window (else in a new tab)", external_new_window),
    D_ACTION(DEFAULT_BROWSER, GENERAL, "Make Aura the Default Browser", AU_PREF_STATUS),
    /* Fonts */
    D_FONT(FONT_SERIF, "fonts.serif", "Serif:", font_serif),
    D_FONT(FONT_SANS, "fonts.sans_serif", "Sans-serif:", font_sans),
    D_FONT(FONT_MONO, "fonts.monospace", "Monospace:", font_mono),
    D_NUMBER(FONT_SIZE, FONTS, "fonts.size", "Default size:", "pixels", 6, 72, font_size),
    D_NUMBER(FONT_SIZE_MONO, FONTS, "fonts.size_monospace", "Monospace size:", "pixels", 6, 72, font_size_mono),
    D_NUMBER(FONT_SIZE_MIN, FONTS, "fonts.minimum_size", "Minimum size:", "pixels (0 for none)", 0, 36,
           font_size_min),
    /* Content */
    D_CHECK(JAVASCRIPT, CONTENT, "content.javascript", "Enable JavaScript", javascript),
    D_CHECK(LOAD_IMAGES, CONTENT, "content.images", "Load images", load_images),
    D_CHECK(BLOCK_POPUPS, CONTENT, "content.block_popups", "Block pop-up windows", block_popups),
    {AU_PREF_SCRIPT_MEMORY_MODE, SEC(CONTENT), AU_PREF_CHOICE, "content.script_memory_kb", "Script memory per page:",
     NULL, NULL, NULL, memory_choices, NULL, 0, 0, AU_PREF_NONE, 0, 0, AU_PREF_STORE_SPECIAL, 0},
    {AU_PREF_SCRIPT_MEMORY_MB, SEC(CONTENT), AU_PREF_NUMBER, NULL, NULL, NULL, "MB", NULL, NULL, NULL,
     (int)(AU_SCRIPT_MEM_CUSTOM_MIN_KB / 1024), (int)(AU_SCRIPT_MEM_CUSTOM_MAX_KB / 1024), AU_PREF_SCRIPT_MEMORY_MODE,
     1, AU_PREF_STATUS, AU_PREF_STORE_SPECIAL, 0},
    {AU_PREF_WORKER_MEMORY_MODE, SEC(CONTENT), AU_PREF_CHOICE, "content.worker_memory_kb",
     "Script memory per worker:", NULL, NULL, NULL, memory_choices, NULL, 0, 0, AU_PREF_NONE, 0, 0,
     AU_PREF_STORE_SPECIAL, 0},
    {AU_PREF_WORKER_MEMORY_MB, SEC(CONTENT), AU_PREF_NUMBER, NULL, NULL, NULL, "MB", NULL, NULL, NULL,
     (int)(AU_SCRIPT_MEM_CUSTOM_MIN_KB / 1024), (int)(AU_SCRIPT_MEM_CUSTOM_MAX_KB / 1024), AU_PREF_WORKER_MEMORY_MODE,
     1, AU_PREF_STATUS, AU_PREF_STORE_SPECIAL, 0},
    {AU_PREF_IMAGE_MEMORY_MODE, SEC(CONTENT), AU_PREF_CHOICE, "content.image_memory_kb", "Image memory per page:",
     NULL, NULL, NULL, memory_choices, NULL, 0, 0, AU_PREF_NONE, 0, 0, AU_PREF_STORE_SPECIAL, 0},
    {AU_PREF_IMAGE_MEMORY_MB, SEC(CONTENT), AU_PREF_NUMBER, NULL, NULL, NULL, "MB", NULL, NULL, NULL,
     (int)(AU_SCRIPT_MEM_CUSTOM_MIN_KB / 1024), (int)(AU_SCRIPT_MEM_CUSTOM_MAX_KB / 1024), AU_PREF_IMAGE_MEMORY_MODE,
     1, AU_PREF_STATUS, AU_PREF_STORE_SPECIAL, 0},
    D_CHOICE(IMAGE_COLORS, CONTENT, AU_PREF_CHOICE, "content.image_colors", "Image colours:", image_colors_choices, 0,
             image_colors),
    D_NOTE(SCRIPT_MEMORY_NOTE, CONTENT, NULL),
    /* Privacy and Security */
    D_CHECK(SEND_DNT, PRIVACY, "privacy.do_not_track", "Send Do Not Track", send_dnt),
    D_CHECK(SEND_GPC, PRIVACY, "privacy.global_privacy_control", "Send Global Privacy Control", send_gpc),
    D_CHECK(HTTPS_FIRST, PRIVACY, "privacy.https_first", "HTTPS-first mode", https_first),
    D_CHECK(LEGACY_TLS, PRIVACY, "security.legacy_tls", "Allow old servers without forward secrecy", legacy_tls),
    D_CHECK(WARN_APP_DOWNLOADS, PRIVACY, "privacy.warn_app_downloads", "Warn before downloading applications",
          warn_app_downloads),
    D_CHOICE(COOKIES, PRIVACY, AU_PREF_CHOICE, "privacy.cookies", "Cookies:", cookie_choices, 0, cookies),
    D_CHECK(BLOCK_THIRD_PARTY_COOKIES, PRIVACY, "privacy.block_third_party_cookies", "Block third-party cookies",
          block_third_party_cookies),
    D_CHECK(HISTORY_ENABLED, PRIVACY, "history.enabled", "Remember history for", history_enabled),
    D_NUMBER(HISTORY_DAYS, PRIVACY, "history.days", NULL, "days", 0, 3650, history_days),
    D_CHECK_CAP(CLEAR_HISTORY_ON_QUIT, PRIVACY, "privacy.clear_history_on_quit", "Clear on quit:", "History",
              clear_history_on_quit),
    D_CHECK(CLEAR_COOKIES_ON_QUIT, PRIVACY, "privacy.clear_cookies_on_quit", "Cookies", clear_cookies_on_quit),
    D_CHECK(CLEAR_CACHE_ON_QUIT, PRIVACY, "privacy.clear_cache_on_quit", "Cache", clear_cache_on_quit),
    D_ACTION(CLEAR_HISTORY, PRIVACY, "Clear History", 0),
    D_ACTION(CLEAR_COOKIES, PRIVACY, "Clear Cookies", 0),
    D_ACTION(CLEAR_CACHE, PRIVACY, "Clear Cache", 0),
    D_ACTION(SHOW_COOKIES, PRIVACY, "Cookies...", 0),
    /* Network */
    D_TEXT(HTTP_PROXY_HOST, NETWORK, "proxy.http.host", "HTTP proxy:", http_proxy_host),
    D_NUMBER(HTTP_PROXY_PORT, NETWORK, "proxy.http.port", "Port:", NULL, 0, 65535, http_proxy_port),
    D_TEXT(HTTPS_PROXY_HOST, NETWORK, "proxy.https.host", "HTTPS proxy:", https_proxy_host),
    D_NUMBER(HTTPS_PROXY_PORT, NETWORK, "proxy.https.port", "Port:", NULL, 0, 65535, https_proxy_port),
    D_NOTE(PROXY_NOTE, NETWORK, "Leave the host empty to connect directly."),
    D_CHOICE(IDENTIFY, NETWORK, AU_PREF_CHOICE, "network.identify", "Identify as:", identify_choices, 0, identify),
    D_CHOICE(UA_OS, NETWORK, AU_PREF_CHOICE, "network.os", "System:", ua_os_choices, 0, ua_os),
    D_CHOICE(UA_CPU, NETWORK, AU_PREF_CHOICE, "network.cpu", "Processor:", ua_cpu_choices, 0, ua_cpu),
    /* shows the string sent; editable for Custom */
    {AU_PREF_USER_AGENT, SEC(NETWORK), AU_PREF_TEXT, "network.user_agent", "User agent:", NULL, NULL, NULL, NULL, NULL,
     0, 0, AU_PREF_IDENTIFY, AU_IDENTIFY_CUSTOM, 0, AU_PREF_STORE_STR, OFF(user_agent)},
    D_NUMBER(MEMORY_CACHE, NETWORK, "cache.memory_kb", "Memory cache:", "KB", 0, 1 << 20, memory_cache_kb),
    D_NUMBER(DISK_CACHE, NETWORK, "cache.disk_kb", "Disk cache:", "KB", 0, 1 << 22, disk_cache_kb),
    D_CHECK(UNLOAD_TABS, NETWORK, "tabs.unload_low_memory", "Unload background tabs when memory is low", unload_tabs),
    /* Downloads */
    {AU_PREF_DOWNLOAD_FOLDER, SEC(DOWNLOADS), AU_PREF_FOLDER, "downloads.folder", "Save files in:", NULL, NULL, NULL,
     NULL, NULL, 0, 0, AU_PREF_NONE, 0, 0, AU_PREF_STORE_STR, OFF(download_folder)},
    D_NOTE(DOWNLOAD_FOLDER_NOTE, DOWNLOADS, NULL),
    D_CHECK(DECODE_DOWNLOADS, DOWNLOADS, "downloads.decode", "Decode MacBinary and BinHex files after downloading",
          decode_downloads),
    D_NOTE(DOWNLOADS_NOTE, DOWNLOADS, "Warning before downloading applications is under Privacy and Security."),
    /* Blocking */
    {AU_PREF_BLOCK_TRACKERS, SEC(BLOCKING), AU_PREF_CHECK, "privacy.block_trackers", NULL,
     "Block ads and trackers with these lists", NULL, NULL, NULL, NULL, 0, 0, AU_PREF_NONE, 0, AU_PREF_LIVE,
     AU_PREF_STORE_BOOL, OFF(block_trackers)},
    {AU_PREF_BLOCK_LISTS, SEC(BLOCKING), AU_PREF_LIST, "blocking.lists", NULL, NULL, NULL, NULL, NULL, NULL, 0, 0,
     AU_PREF_NONE, 0, AU_PREF_LIVE | AU_PREF_STATUS, AU_PREF_STORE_STR, OFF(block_lists)},
    D_CHOICE(BLOCK_UPDATE, BLOCKING, AU_PREF_CHOICE, "blocking.update", "Look for updates:", update_choices, 0,
           block_update),
    D_ACTION(BLOCK_UPDATE_NOW, BLOCKING, "Update Now", 0),
    {AU_PREF_BLOCK_CUSTOM, SEC(BLOCKING), AU_PREF_TEXT, "blocking.custom", "More lists:", NULL, NULL, NULL, NULL,
     NULL, 0, 0, AU_PREF_NONE, 0, AU_PREF_STATUS, AU_PREF_STORE_STR, OFF(block_custom)},
    D_TEXT(BLOCK_ALLOW, BLOCKING, "blocking.allow", "Always allow:", block_allow),
    D_TEXT(BLOCK_DENY, BLOCKING, "blocking.block", "Always block:", block_deny),
};

static const char *const section_titles[AU_PREF_SECTION_COUNT] = {
    "General", "Fonts", "Content", "Privacy and Security", "Network", "Downloads", "Blocking"};

const AuPrefDesc *au_pref_desc(AuPrefId id)
{
    return id >= 0 && id < AU_PREF_COUNT ? &descs[id] : NULL;
}

const AuPrefDesc *au_pref_desc_by_key(const char *key)
{
    for (int i = 0; i < AU_PREF_COUNT; i++)
        if (descs[i].key && !strcmp(descs[i].key, key))
            return &descs[i];
    return NULL;
}

const char *au_pref_section_title(AuPrefSection s)
{
    return s >= 0 && s < AU_PREF_SECTION_COUNT ? section_titles[s] : "";
}

size_t au_pref_choice_count(const AuPrefDesc *d)
{
    size_t n = 0;
    if (d && d->choices)
        while (d->choices[n])
            n++;
    return n;
}

/* ---- The session ---- */

enum { H_MACHINE, H_SETTINGS, H_EXAMPLE, H_DOWNLOADS, H_NO_FOLDER, H_COUNT };

struct AuPrefsEdit {
    AuPrefs work, orig;
    AuPrefsHost host;
    char *strs[H_COUNT];
    char **families;
    size_t nfamilies;
    /* Script memory as the window shows it: Automatic (0) or Custom (1),
     * and the number of megabytes in the field. */
    int mem_mode[3], mem_mb[3]; /* script per page, per worker, images */
};

/* Enum fields are accessed as int (prefs.c checks they are int-sized). */
static bool *bool_at(AuPrefs *p, const AuPrefDesc *d) { return (bool *)((char *)p + d->offset); }
static int *int_at(AuPrefs *p, const AuPrefDesc *d) { return (int *)((char *)p + d->offset); }
static char **str_at(AuPrefs *p, const AuPrefDesc *d) { return (char **)((char *)p + d->offset); }

static const AuPrefDesc *desc_of(AuPrefId id) { return au_pref_desc(id); }

static int mem_slot(AuPrefId id)
{
    switch (id) {
    case AU_PREF_SCRIPT_MEMORY_MODE:
    case AU_PREF_SCRIPT_MEMORY_MB: return 0;
    case AU_PREF_WORKER_MEMORY_MODE:
    case AU_PREF_WORKER_MEMORY_MB: return 1;
    case AU_PREF_IMAGE_MEMORY_MODE:
    case AU_PREF_IMAGE_MEMORY_MB: return 2;
    default: return -1;
    }
}

static uint32_t auto_kb(const AuPrefsEdit *e, int slot)
{
    if (slot == 2)
        return au_image_memory_auto(e->host.memory_free_kb);
    uint32_t page_kb, worker_kb;
    au_script_memory_auto(e->host.memory_free_kb, &page_kb, &worker_kb);
    return slot ? worker_kb : page_kb;
}

/* The preference field of a memory slot. */
static int *mem_field(AuPrefs *p, int slot)
{
    return slot == 2 ? &p->image_memory_kb : slot ? &p->worker_memory_kb : &p->script_memory_kb;
}

/* Automatic and Unlimited show what Automatic gives now, ready to edit
 * after a switch to Custom. */
static void load_memory(AuPrefsEdit *e, int slot, int kb)
{
    e->mem_mode[slot] = kb < 0 ? 2 : kb ? 1 : 0;
    e->mem_mb[slot] = (int)((kb > 0 ? (uint32_t)kb : auto_kb(e, slot)) / 1024);
}

static char *dup_or_null(const char *s)
{
    return s ? au_strndup(s, strlen(s)) : NULL;
}

static void collect_families(AuPrefsEdit *e, const AuFontSet *fonts)
{
    size_t n = fonts ? au_fontset_count(fonts) : 0;
    e->families = au_alloc((n + 1) * sizeof *e->families);
    e->nfamilies = 0;
    for (size_t i = 0; i < n; i++) {
        const char *fam = au_fontset_entry(fonts, i, NULL, NULL);
        if (!fam)
            continue;
        bool seen = false;
        for (size_t k = 0; k < e->nfamilies && !seen; k++)
            seen = !strcmp(e->families[k], fam);
        if (!seen)
            e->families[e->nfamilies++] = au_strndup(fam, strlen(fam));
    }
    /* Sort by name: an insertion sort is plenty for a few dozen. */
    for (size_t i = 1; i < e->nfamilies; i++) {
        char *x = e->families[i];
        size_t j = i;
        while (j > 0 && strcmp(e->families[j - 1], x) > 0) {
            e->families[j] = e->families[j - 1];
            j--;
        }
        e->families[j] = x;
    }
}

AuPrefsEdit *au_prefs_edit_new(const AuPrefs *prefs, const AuPrefsHost *host)
{
    AuPrefsEdit *e = au_calloc(sizeof *e);
    au_prefs_copy(&e->work, prefs);
    au_prefs_copy(&e->orig, prefs);
    if (host)
        e->host = *host;
    e->strs[H_MACHINE] = dup_or_null(e->host.machine);
    e->strs[H_SETTINGS] = dup_or_null(e->host.settings_name);
    e->strs[H_EXAMPLE] = dup_or_null(e->host.folder_example);
    e->strs[H_DOWNLOADS] = dup_or_null(e->host.system_downloads);
    e->strs[H_NO_FOLDER] = dup_or_null(e->host.no_folder);
    e->host.machine = e->strs[H_MACHINE];
    e->host.settings_name = e->strs[H_SETTINGS];
    e->host.folder_example = e->strs[H_EXAMPLE];
    e->host.system_downloads = e->strs[H_DOWNLOADS];
    e->host.no_folder = e->strs[H_NO_FOLDER];
    collect_families(e, e->host.fonts);
    load_memory(e, 0, prefs->script_memory_kb);
    load_memory(e, 1, prefs->worker_memory_kb);
    load_memory(e, 2, prefs->image_memory_kb);
    return e;
}

void au_prefs_edit_free(AuPrefsEdit *e)
{
    if (!e)
        return;
    au_prefs_free(&e->work);
    au_prefs_free(&e->orig);
    for (int i = 0; i < H_COUNT; i++)
        au_free(e->strs[i]);
    for (size_t i = 0; i < e->nfamilies; i++)
        au_free(e->families[i]);
    au_free(e->families);
    au_free(e);
}

const AuPrefs *au_prefs_edit_prefs(const AuPrefsEdit *e)
{
    return &e->work;
}

void au_prefs_edit_set_default_browser(AuPrefsEdit *e, AuDefaultBrowser state)
{
    e->host.default_browser = state;
}

void au_prefs_edit_set_memory_free(AuPrefsEdit *e, uint32_t free_kb)
{
    e->host.memory_free_kb = free_kb;
}

/* ---- Values ---- */

bool au_prefs_edit_get_bool(const AuPrefsEdit *e, AuPrefId id)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d || d->store != AU_PREF_STORE_BOOL)
        return false;
    return *bool_at((AuPrefs *)&e->work, d);
}

bool au_prefs_edit_set_bool(AuPrefsEdit *e, AuPrefId id, bool v)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d || d->store != AU_PREF_STORE_BOOL)
        return false;
    *bool_at(&e->work, d) = v;
    return true;
}

int au_prefs_edit_get_int(const AuPrefsEdit *e, AuPrefId id)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d || d->kind != AU_PREF_NUMBER)
        return 0;
    int slot = mem_slot(id);
    if (slot >= 0)
        return e->mem_mb[slot];
    return *int_at((AuPrefs *)&e->work, d);
}

bool au_prefs_edit_set_int(AuPrefsEdit *e, AuPrefId id, long v)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d || d->kind != AU_PREF_NUMBER)
        return false;
    if (v < d->min)
        v = d->min;
    if (v > d->max)
        v = d->max;
    int slot = mem_slot(id);
    if (slot >= 0)
        e->mem_mb[slot] = (int)v;
    else
        *int_at(&e->work, d) = (int)v;
    return true;
}

static size_t choice_count(const AuPrefsEdit *e, const AuPrefDesc *d)
{
    return d->kind == AU_PREF_FONT ? e->nfamilies : au_pref_choice_count(d);
}

int au_prefs_edit_get_choice(const AuPrefsEdit *e, AuPrefId id)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d)
        return -1;
    if (d->kind == AU_PREF_FONT) {
        const char *fam = *str_at((AuPrefs *)&e->work, d);
        int found = -1;
        for (size_t i = 0; i < e->nfamilies; i++)
            if (au_ieq(e->families[i], strlen(e->families[i]), fam, strlen(fam)))
                found = (int)i; /* the last match, as the menu was set before */
        return found;
    }
    if (d->kind != AU_PREF_CHOICE && d->kind != AU_PREF_RADIO)
        return -1;
    int slot = mem_slot(id);
    if (slot >= 0)
        return e->mem_mode[slot];
    int v = *int_at((AuPrefs *)&e->work, d);
    return v >= 0 && (size_t)v < choice_count(e, d) ? v : -1;
}

bool au_prefs_edit_set_choice(AuPrefsEdit *e, AuPrefId id, int index)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d)
        return false;
    int n = (int)choice_count(e, d);
    if (d->kind == AU_PREF_FONT) {
        if (index < 0 || index >= n)
            return false;
        au_prefs_set_str(str_at(&e->work, d), e->families[index]);
        return true;
    }
    if (d->kind != AU_PREF_CHOICE && d->kind != AU_PREF_RADIO)
        return false;
    if (id == AU_PREF_BLOCK_UPDATE) {
        if (index < 0 || index >= n)
            index = AU_BLOCK_UPDATE_DAILY;
    } else {
        if (index < 0)
            index = 0;
        if (index > n - 1)
            index = n - 1;
    }
    int slot = mem_slot(id);
    if (slot >= 0)
        e->mem_mode[slot] = index;
    else
        *int_at(&e->work, d) = index;
    return true;
}

void au_prefs_edit_get_text(const AuPrefsEdit *e, AuPrefId id, AuBuf *out)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d)
        return;
    if (d->kind == AU_PREF_NUMBER) {
        au_buf_printf(out, "%d", au_prefs_edit_get_int(e, id));
    } else if (id == AU_PREF_USER_AGENT) {
        au_buf_puts(out, au_prefs_user_agent(&e->work));
    } else if (d->store == AU_PREF_STORE_STR) {
        au_buf_puts(out, *str_at((AuPrefs *)&e->work, d));
    }
}

/* Appends the https addresses in text to out, separated by spaces. */
void au_prefs_clean_urls(const char *text, AuBuf *out)
{
    for (const char *s = text; *s;) {
        while (*s && (unsigned char)*s <= ' ')
            s++;
        size_t n = 0;
        while (s[n] && (unsigned char)s[n] > ' ')
            n++;
        if (n > 11 && n < 1024 && au_ieq(s, 8, "https://", 8)) {
            if (out->len)
                au_buf_putc(out, ' ');
            au_buf_append(out, s, n);
        }
        s += n;
    }
}

/* The same for domains (commas also separate). */
void au_prefs_clean_domains(const char *text, AuBuf *out)
{
    for (const char *s = text; *s;) {
        while (*s && ((unsigned char)*s <= ' ' || *s == ','))
            s++;
        size_t n = 0;
        while (s[n] && (unsigned char)s[n] > ' ' && s[n] != ',')
            n++;
        AuBuf d;
        au_buf_init(&d);
        if (n && au_block_clean_domain(s, n, &d) && !au_block_words_has(au_buf_cstr(out), au_buf_cstr(&d))) {
            if (out->len)
                au_buf_putc(out, ' ');
            au_buf_append(out, d.data, d.len);
        }
        au_buf_free(&d);
        s += n;
    }
}

static bool parse_number(const char *s, long *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end)
        return false;
    *out = v;
    return true;
}

bool au_prefs_edit_set_text(AuPrefsEdit *e, AuPrefId id, const char *text)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d || !text)
        return false;
    switch (d->kind) {
    case AU_PREF_NUMBER: {
        long v;
        if (parse_number(text, &v))
            return au_prefs_edit_set_int(e, id, v);
        /* Not a number: the value the preference had when the window opened. */
        int slot = mem_slot(id);
        if (slot >= 0) {
            int kb = *mem_field((AuPrefs *)&e->orig, slot);
            au_prefs_edit_set_int(e, id, kb > 0 ? kb / 1024 : 0); /* Automatic or Unlimited: the floor */
        }
        else
            au_prefs_edit_set_int(e, id, *int_at(&e->orig, d));
        return false;
    }
    case AU_PREF_FONT:
        for (size_t i = 0; i < e->nfamilies; i++)
            if (au_ieq(e->families[i], strlen(e->families[i]), text, strlen(text)))
                return au_prefs_edit_set_choice(e, id, (int)i);
        return false;
    case AU_PREF_TEXT:
    case AU_PREF_FOLDER:
        break;
    default:
        return false;
    }
    char **field = str_at(&e->work, d);
    if (id == AU_PREF_BLOCK_CUSTOM || id == AU_PREF_BLOCK_ALLOW || id == AU_PREF_BLOCK_DENY) {
        AuBuf b;
        au_buf_init(&b);
        if (id == AU_PREF_BLOCK_CUSTOM)
            au_prefs_clean_urls(text, &b);
        else
            au_prefs_clean_domains(text, &b);
        au_prefs_set_str(field, au_buf_cstr(&b));
        au_buf_free(&b);
    } else if (id == AU_PREF_USER_AGENT) {
        /* The field shows the string sent; only Custom keeps what it
         * holds. Aura's own string is stored as empty, so it follows
         * Aura's version, and clearing the field returns to it. */
        if (e->work.identify != AU_IDENTIFY_CUSTOM)
            return true;
        au_prefs_set_str(field, strcmp(text, au_aura_user_agent()) && strcmp(text, AU_DEFAULT_USER_AGENT) ? text : "");
        if (!e->work.user_agent[0])
            e->work.identify = AU_IDENTIFY_AURA;
    } else {
        au_prefs_set_str(field, text);
    }
    return true;
}

size_t au_prefs_edit_choice_count(const AuPrefsEdit *e, AuPrefId id)
{
    const AuPrefDesc *d = desc_of(id);
    return d ? choice_count(e, d) : 0;
}

const char *au_prefs_edit_choice(const AuPrefsEdit *e, AuPrefId id, size_t i)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d || i >= choice_count(e, d))
        return NULL;
    return d->kind == AU_PREF_FONT ? e->families[i] : d->choices[i];
}

bool au_prefs_edit_enabled(const AuPrefsEdit *e, AuPrefId id)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d)
        return false;
    if (id == AU_PREF_DEFAULT_BROWSER)
        return e->host.default_browser != AU_DEFAULT_BROWSER_UNAVAILABLE;
    if (d->enabled_by != AU_PREF_NONE)
        return au_prefs_edit_get_choice(e, d->enabled_by) == d->enabled_value;
    return true;
}

/* "91,414". */
static void group_digits(char *out, size_t cap, unsigned long v)
{
    char raw[24];
    int n = snprintf(raw, sizeof raw, "%lu", v);
    size_t k = 0;
    for (int i = 0; i < n && k + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0)
            out[k++] = ',';
        out[k++] = raw[i];
    }
    out[k] = 0;
}

static const char *const month_abbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void au_prefs_block_status(const AuBlockListInfo *li, char *out, size_t cap)
{
    char count[24];
    group_digits(count, sizeof count, li->entries);
    if (li->state == AU_BLOCK_DOWNLOADING) {
        snprintf(out, cap, "downloading, %ld KB so far", (long)(li->received / 1024));
    } else if (li->state == AU_BLOCK_COMPILING) {
        snprintf(out, cap, "compiling");
    } else if (li->error[0] && !li->loaded) {
        snprintf(out, cap, "not loaded: %s", li->error);
    } else if (li->state == AU_BLOCK_WAITING && !li->loaded) {
        snprintf(out, cap, "waiting to download");
    } else if (li->loaded) {
        int y, m, d, wd;
        au_date_of_day(au_day_of(li->updated_at), &y, &m, &d, &wd);
        if (li->updated_at)
            snprintf(out, cap, "%s domains, %lu KB, %d %s%s%s", count, (unsigned long)((li->bytes + 512) / 1024), d,
                     month_abbr[(m + 11) % 12], li->error[0] ? "; " : "", li->error);
        else
            snprintf(out, cap, "%s domains, %lu KB", count, (unsigned long)((li->bytes + 512) / 1024));
    } else {
        snprintf(out, cap, "about %s domains, %lu KB", count,
                 (unsigned long)(((unsigned long)li->entries * AU_BLOCK_HASH_BYTES + 512) / 1024));
    }
}

/* The status line under More lists: the lists added by address. */
static void custom_status(const AuPrefsEdit *e, AuBuf *out)
{
    AuBlockManager *m = e->host.blocking;
    if (!m)
        return;
    AuBlockListInfo li;
    unsigned long custom = 0, custom_loaded = 0, custom_domains = 0;
    char custom_error[96] = "", custom_doing[48] = "";
    for (size_t i = 0; au_block_manager_info(m, i, &li); i++) {
        if (li.catalog)
            continue;
        custom++;
        if (li.loaded) {
            custom_loaded++;
            custom_domains += li.entries;
        }
        if (li.error[0] && !custom_error[0])
            snprintf(custom_error, sizeof custom_error, "%s", li.error);
        if (li.state == AU_BLOCK_DOWNLOADING || li.state == AU_BLOCK_COMPILING)
            snprintf(custom_doing, sizeof custom_doing, "; %s",
                     li.state == AU_BLOCK_COMPILING ? "compiling" : "downloading");
    }
    if (!custom) {
        au_buf_puts(out, "Addresses of other lists (https), separated by spaces.");
        return;
    }
    char count[24];
    group_digits(count, sizeof count, custom_domains);
    au_buf_printf(out, "%lu of %lu loaded, %s domains%s%s%s", custom_loaded, custom, count, custom_doing,
                  custom_error[0] ? "; " : "", custom_error);
}

static const char *or_default(const char *s, const char *dflt)
{
    return s && *s ? s : dflt;
}

void au_prefs_edit_text(const AuPrefsEdit *e, AuPrefId id, AuBuf *out)
{
    const AuPrefDesc *d = desc_of(id);
    if (!d)
        return;
    const char *machine = or_default(e->host.machine, "this computer");
    switch (id) {
    case AU_PREF_SCRIPT_MEMORY_MB:
    case AU_PREF_WORKER_MEMORY_MB:
    case AU_PREF_IMAGE_MEMORY_MB:
        au_buf_printf(out, "Automatic is %lu MB on %s", (unsigned long)(auto_kb(e, mem_slot(id)) / 1024), machine);
        return;
    case AU_PREF_SCRIPT_MEMORY_NOTE:
        au_buf_printf(out,
                      "How much memory the scripts of one page, each worker a page starts, and a page's decoded "
                      "images may use. Automatic follows the memory free on %s when a page starts. A custom limit is "
                      "%u to %u MB; Unlimited sets none. A page whose script reaches its limit says so in the Console; images past theirs "
                      "are decoded again when shown. Changes apply to pages loaded afterwards.",
                      machine, AU_SCRIPT_MEM_CUSTOM_MIN_KB / 1024, AU_SCRIPT_MEM_CUSTOM_MAX_KB / 1024);
        return;
    case AU_PREF_DEFAULT_BROWSER:
        if (e->host.default_browser == AU_DEFAULT_BROWSER_UNAVAILABLE)
            au_buf_printf(out, "%s is not available.", or_default(e->host.settings_name, "The system setting"));
        else if (e->host.default_browser == AU_DEFAULT_BROWSER_YES)
            au_buf_puts(out, "Aura is the default.");
        return;
    case AU_PREF_DOWNLOAD_FOLDER_NOTE: {
        const char *example = or_default(e->host.folder_example, "a full path");
        if (e->host.system_downloads)
            au_buf_printf(out, "A folder path such as %s. Leave it empty for %s's download folder (%s).", example,
                          or_default(e->host.settings_name, "the system"), e->host.system_downloads);
        else
            au_buf_printf(out, "A folder path such as %s. Leave it empty for %s.", example,
                          or_default(e->host.no_folder, "the desktop"));
        return;
    }
    case AU_PREF_BLOCK_CUSTOM:
        custom_status(e, out);
        return;
    default:
        if (d->help)
            au_buf_puts(out, d->help);
        return;
    }
}

/* ---- Sections, live items, applying ---- */

static bool copy_item(const AuPrefDesc *d, AuPrefs *dst, const AuPrefs *src)
{
    AuPrefs *s = (AuPrefs *)src;
    switch (d->store) {
    case AU_PREF_STORE_BOOL:
        if (*bool_at(dst, d) == *bool_at(s, d))
            return false;
        *bool_at(dst, d) = *bool_at(s, d);
        return true;
    case AU_PREF_STORE_INT:
    case AU_PREF_STORE_ENUM:
        if (*int_at(dst, d) == *int_at(s, d))
            return false;
        *int_at(dst, d) = *int_at(s, d);
        return true;
    case AU_PREF_STORE_STR:
        if (!strcmp(*str_at(dst, d), *str_at(s, d)))
            return false;
        au_prefs_set_str(str_at(dst, d), *str_at(s, d));
        return true;
    default:
        return false;
    }
}

void au_prefs_edit_reset_section(AuPrefsEdit *e, AuPrefSection s)
{
    AuPrefs def;
    au_prefs_init(&def);
    for (int i = 0; i < AU_PREF_COUNT; i++)
        if (descs[i].section == s)
            copy_item(&descs[i], &e->work, &def);
    if (s == AU_PREF_SECTION_CONTENT) {
        load_memory(e, 0, def.script_memory_kb);
        load_memory(e, 1, def.worker_memory_kb);
        load_memory(e, 2, def.image_memory_kb);
    }
    au_prefs_free(&def);
}

/* The live items between two preference sets. */
static unsigned copy_live(AuPrefs *dst, const AuPrefs *src, AuBrowser *b)
{
    unsigned bits = 0;
    bool blocking = copy_item(&descs[AU_PREF_BLOCK_LISTS], dst, src);
    if (copy_item(&descs[AU_PREF_BLOCK_TRACKERS], dst, src))
        blocking = true;
    if (blocking) {
        b->prefs_gen++;
        bits |= AU_PREF_CHANGED_BLOCKING;
    }
    if (copy_item(&descs[AU_PREF_TAB_POSITION], dst, src)) {
        au_browser_prefs_changed(b);
        bits |= AU_PREF_CHANGED_TABS;
    }
    if (copy_item(&descs[AU_PREF_APPEARANCE], dst, src)) {
        au_browser_prefs_changed(b); /* every page takes the new scheme */
        bits |= AU_PREF_CHANGED_APPEARANCE;
    }
    return bits;
}

unsigned au_prefs_edit_apply_live(AuPrefsEdit *e, AuBrowser *b)
{
    return copy_live(&b->prefs, &e->work, b);
}

unsigned au_prefs_edit_cancel(AuPrefsEdit *e, AuBrowser *b)
{
    unsigned bits = copy_live(&b->prefs, &e->orig, b);
    copy_live(&e->work, &e->orig, b);
    return bits;
}

bool au_prefs_edit_apply(const AuPrefsEdit *e, AuPrefs *prefs)
{
    bool changed = false;
    for (int i = 0; i < AU_PREF_COUNT; i++)
        if (copy_item(&descs[i], prefs, &e->work))
            changed = true;
    /* Automatic is stored as 0, so it follows Aura's policy; Unlimited as
     * AU_MEM_UNLIMITED. */
    for (int slot = 0; slot < 3; slot++) {
        int kb = e->mem_mode[slot] == 2   ? AU_MEM_UNLIMITED
                 : e->mem_mode[slot] == 1 ? au_script_memory_clamp(e->mem_mb[slot] * 1024)
                                          : 0;
        int *field = mem_field(prefs, slot);
        if (*field != kb) {
            *field = kb;
            changed = true;
        }
    }
    return changed;
}

bool au_prefs_edit_commit(AuPrefsEdit *e, AuBrowser *b)
{
    bool changed = au_prefs_edit_apply(e, &b->prefs);
    au_browser_prefs_changed(b);
    return changed;
}

unsigned au_prefs_edit_action(AuPrefsEdit *e, AuPrefId id, AuBrowser *b, AuBrowserWindow *front,
                              AuPrefId *changed)
{
    if (changed)
        *changed = AU_PREF_NONE;
    switch (id) {
    case AU_PREF_USE_CURRENT_PAGE: {
        AuTab *t = front ? au_bw_active_tab(front) : NULL;
        if (!t)
            return 0;
        au_prefs_set_str(&e->work.home_page, au_page_url(t->page));
        if (changed)
            *changed = AU_PREF_HOME_PAGE;
        return AU_PREF_DO_RELOAD;
    }
    case AU_PREF_DEFAULT_BROWSER:
        return AU_PREF_DO_MAKE_DEFAULT;
    case AU_PREF_CLEAR_HISTORY:
        au_history_clear(&b->history);
        au_favicons_clear(b->favicons); /* they name visited hosts */
        au_browser_history_changed(b);
        return 0;
    case AU_PREF_CLEAR_COOKIES:
        return AU_PREF_DO_CLEAR_COOKIES;
    case AU_PREF_CLEAR_CACHE:
        au_browser_release_documents(b); /* the documents kept for Back and Forward */
        return AU_PREF_DO_CLEAR_CACHE;
    case AU_PREF_SHOW_COOKIES:
        return AU_PREF_DO_SHOW_COOKIES;
    case AU_PREF_BLOCK_UPDATE_NOW:
        if (e->host.blocking)
            au_block_manager_update_now(e->host.blocking);
        return AU_PREF_DO_REFRESH_LISTS;
    default:
        return 0;
    }
}

/* ---- The blocking lists ---- */

size_t au_prefs_edit_block_list_count(const AuPrefsEdit *e)
{
    (void)e;
    size_t n;
    au_block_catalog(&n);
    return n;
}

bool au_prefs_edit_block_list(const AuPrefsEdit *e, size_t i, AuPrefBlockRow *out)
{
    size_t n;
    const AuBlockCatalogEntry *cat = au_block_catalog(&n);
    if (i >= n)
        return false;
    out->id = cat[i].id;
    out->title = cat[i].title;
    out->on = au_block_words_has(e->work.block_lists, cat[i].id);
    out->status[0] = 0;
    AuBlockManager *m = e->host.blocking;
    AuBlockListInfo li;
    for (size_t k = 0; m && au_block_manager_info(m, k, &li); k++)
        if (li.catalog && !strcmp(li.id, cat[i].id))
            au_prefs_block_status(&li, out->status, sizeof out->status);
    return true;
}

bool au_prefs_edit_set_block_list(AuPrefsEdit *e, size_t i, bool on)
{
    size_t n;
    const AuBlockCatalogEntry *cat = au_block_catalog(&n);
    if (i >= n)
        return false;
    /* The lists in catalogue order, as the check boxes show them. */
    AuBuf b;
    au_buf_init(&b);
    for (size_t k = 0; k < n; k++) {
        bool has = k == i ? on : au_block_words_has(e->work.block_lists, cat[k].id);
        if (has) {
            if (b.len)
                au_buf_putc(&b, ' ');
            au_buf_puts(&b, cat[k].id);
        }
    }
    au_prefs_set_str(&e->work.block_lists, au_buf_cstr(&b));
    au_buf_free(&b);
    return true;
}

uint32_t au_prefs_edit_block_generation(const AuPrefsEdit *e)
{
    return e->host.blocking ? au_block_manager_generation(e->host.blocking) : 0;
}
