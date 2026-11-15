/* Browser preferences: a typed struct, its defaults, and the key=value
 * text file it is stored in. Portable C99; the platform reads and writes
 * the file and passes the bytes here.
 *
 * To read a setting, read the field of AuPrefs. Settings whose subsystems
 * do not exist yet (JavaScript, images, cookies, privacy, proxies, caches,
 * downloads) are here already, so the preferences window can show and
 * edit them and the subsystems read them from this struct when they
 * arrive.
 *
 * File format: UTF-8 text, one "key=value" per line. Lines starting with
 * '#' and blank lines are ignored. In values, "\n" is a newline and "\\"
 * a backslash. Booleans are "true"/"false", numbers decimal, enums by
 * name (see the table in prefs.c). Keys Aura does not know are kept, in
 * order, and written back unchanged, so a newer version's settings
 * survive an older one. A missing key keeps its default; an out-of-range
 * value is clamped. */
#ifndef AURA_BROWSER_PREFS_H
#define AURA_BROWSER_PREFS_H

#include "../core/core.h"

typedef enum { AU_TABS_TOP, AU_TABS_LEFT } AuTabPosition;
typedef enum { AU_STARTUP_HOME, AU_STARTUP_BLANK, AU_STARTUP_RESTORE } AuStartup;
typedef enum { AU_COOKIES_ALL, AU_COOKIES_SAME_SITE, AU_COOKIES_NONE } AuCookiePolicy;
typedef enum { AU_BLOCK_UPDATE_DAILY, AU_BLOCK_UPDATE_WEEKLY, AU_BLOCK_UPDATE_NEVER } AuBlockUpdate;

/* Limits the window and the file parser apply. */
#define AU_TAB_COLUMN_MIN 100
#define AU_TAB_COLUMN_MAX 400

typedef struct AuPrefsExtra {
    char *key, *value;
} AuPrefsExtra;

typedef struct AuPrefs {
    /* General */
    char *home_page;
    AuStartup startup;
    AuTabPosition tab_position;
    int tab_column_width; /* pixels, for AU_TABS_LEFT */
    char *search_template; /* "%s" is replaced by the query */

    /* Fonts: family names and sizes in pixels */
    char *font_serif, *font_sans, *font_mono;
    int font_size, font_size_mono, font_size_min;

    /* Content */
    bool javascript;
    bool load_images;
    bool block_popups;

    /* Privacy and security */
    AuCookiePolicy cookies;
    bool block_third_party_cookies;
    bool send_dnt;      /* DNT: 1 */
    bool send_gpc;      /* Sec-GPC: 1 */
    bool https_first;   /* try HTTPS, warn before falling back to HTTP */
    bool block_trackers; /* host list of trackers and advertisers */
    bool warn_app_downloads;
    bool clear_history_on_quit, clear_cookies_on_quit, clear_cache_on_quit;
    bool history_enabled;
    int history_days;

    /* Network */
    char *http_proxy_host;
    int http_proxy_port;
    char *https_proxy_host;
    int https_proxy_port;
    char *user_agent;
    int memory_cache_kb, disk_cache_kb;

    /* Downloads */
    char *download_folder; /* platform path; "" for the default */
    bool decode_downloads; /* decode MacBinary and BinHex after download */

    /* Window size remembered between runs (content area, pixels; 0 for
     * the default). */
    int window_width, window_height;

    /* Text zoom remembered per site: "key=percent" items separated by
     * spaces (au_browser_site_zoom). */
    char *zoom_sites;
    /* Cookie rules per site: "site=allow|session|block" items separated
     * by spaces, keyed by registrable domain (au_cookie_rules_parse). */
    char *cookie_sites;
    /* URLs other applications open (the GURL Apple event) go to a new
     * window rather than a new tab in the front window. */
    bool external_new_window;

    /* Content blocking (block_trackers is the switch; src/net/blocklist.h).
     * block_lists: ids of the catalogue lists in use, separated by spaces.
     * block_custom: https URLs of lists the user added. block_allow and
     * block_deny: the user's own domains, never and always blocked.
     * block_sites_off: registrable domains of sites where nothing is
     * blocked. */
    char *block_lists, *block_custom, *block_allow, *block_deny, *block_sites_off;
    AuBlockUpdate block_update; /* how often the lists' publishers are asked */

    /* Keys this version does not know, kept for writing back. */
    AuPrefsExtra *extra;
    size_t nextra;
} AuPrefs;

void au_prefs_init(AuPrefs *p); /* defaults */
void au_prefs_free(AuPrefs *p);
void au_prefs_copy(AuPrefs *dst, const AuPrefs *src); /* dst must be free or uninitialised */

/* Sets a string field (frees the old value). */
void au_prefs_set_str(char **field, const char *value);

/* Parses a preferences file into p, which holds defaults or earlier
 * values. Returns false when the file looks damaged (binary bytes, or
 * lines that are not key=value): what could be read is still applied,
 * and the caller keeps the damaged file aside rather than overwriting it
 * silently. */
bool au_prefs_parse(AuPrefs *p, const char *data, size_t len);
void au_prefs_serialize(const AuPrefs *p, AuBuf *out);

/* Reads or writes one setting by its file key, as text. Returns false
 * for an unknown key (au_prefs_set then keeps it as an extra) or, for
 * au_prefs_get, writes "" to out. */
bool au_prefs_set(AuPrefs *p, const char *key, const char *value);
bool au_prefs_get(const AuPrefs *p, const char *key, AuBuf *out);

#endif
