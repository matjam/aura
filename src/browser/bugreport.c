/* The bug report (see bugreport.h). */
#include "bugreport.h"
#include "context.h" /* au_bw_site_blocking */
#include "netlog.h"

#include <stdio.h>
#include <string.h>

/* ---- Redaction ---- */

static bool starts_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (a != b)
            return false;
    }
    return true;
}

/* Appends s[0..n), cut to AU_BUGREPORT_QUERY_MAX bytes with "..." when
 * longer (on a character boundary). */
static void put_cut(AuBuf *out, const char *s, size_t n)
{
    if (n <= AU_BUGREPORT_QUERY_MAX) {
        au_buf_append(out, s, n);
        return;
    }
    size_t k = AU_BUGREPORT_QUERY_MAX;
    while (k && ((unsigned char)s[k] & 0xC0) == 0x80)
        k--;
    au_buf_append(out, s, k);
    au_buf_puts(out, "...");
}

static void redact_url_n(const char *url, size_t len, AuBuf *out)
{
    /* A file: address names a place on the person's disk: only the file
     * name stays. */
    if (starts_ci(url, "file:")) {
        size_t end = 0;
        while (end < len && url[end] != '?' && url[end] != '#')
            end++;
        size_t name = end;
        while (name > 5 && url[name - 1] != '/')
            name--;
        au_buf_puts(out, "file:///.../");
        au_buf_append(out, url + name, end - name);
        return;
    }
    size_t i = 0;
    const char *sep = NULL;
    for (size_t k = 0; k + 2 < len; k++) {
        if (url[k] == ':' && url[k + 1] == '/' && url[k + 2] == '/') {
            sep = url + k;
            break;
        }
        if (url[k] == '/' || url[k] == '?' || url[k] == '#')
            break;
    }
    if (sep) {
        /* scheme://user:password@host: the user name and password go. */
        size_t host = (size_t)(sep - url) + 3, at = 0;
        for (size_t k = host; k < len && url[k] != '/' && url[k] != '?' && url[k] != '#'; k++)
            if (url[k] == '@')
                at = k + 1;
        au_buf_append(out, url, host);
        i = at ? at : host;
    }
    size_t q = i;
    while (q < len && url[q] != '?' && url[q] != '#')
        q++;
    au_buf_append(out, url + i, q - i);
    if (q < len && url[q] == '?') {
        size_t e = q + 1;
        while (e < len && url[e] != '#')
            e++;
        au_buf_putc(out, '?');
        put_cut(out, url + q + 1, e - q - 1);
        q = e;
    }
    if (q < len && url[q] == '#') {
        au_buf_putc(out, '#');
        put_cut(out, url + q + 1, len - q - 1);
    }
}

void au_bugreport_redact_url(const char *url, AuBuf *out)
{
    redact_url_n(url, strlen(url), out);
}

void au_bugreport_redact_text(const char *text, AuBuf *out)
{
    const char *s = text;
    while (*s) {
        if (starts_ci(s, "http://") || starts_ci(s, "https://") || starts_ci(s, "file:/") || starts_ci(s, "ftp://")) {
            /* The address runs to white space, a quote or an angle
             * bracket; a closing bracket or punctuation mark at its end
             * belongs to the text around it. */
            size_t n = strcspn(s, " \t\r\n\"'<>`");
            size_t keep = n;
            while (keep && strchr(").,;:]", s[keep - 1]))
                keep--;
            redact_url_n(s, keep, out);
            s += keep;
            continue;
        }
        au_buf_putc(out, *s++);
    }
}

void au_bugreport_ascii(const char *text, size_t len, AuBuf *out)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            if (c == '`' && i + 2 < len && text[i + 1] == '`' && text[i + 2] == '`') {
                au_buf_puts(out, "'''");
                i += 3;
                continue;
            }
            if (c == '\r') {
                /* CR LF and a lone CR are one line break. */
                if (!(i + 1 < len && text[i + 1] == '\n'))
                    au_buf_putc(out, '\n');
            } else if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F)) {
                au_buf_putc(out, (char)c);
            } else {
                au_buf_putc(out, '?');
            }
            i++;
            continue;
        }
        size_t adv = 1;
        uint32_t cp = au_utf8_decode(text + i, len - i, &adv);
        i += adv;
        switch (cp) {
        case 0x00A0: au_buf_putc(out, ' '); break;
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014:
        case 0x2212: au_buf_putc(out, '-'); break;
        case 0x2018:
        case 0x2019: au_buf_putc(out, '\''); break;
        case 0x201C:
        case 0x201D: au_buf_putc(out, '"'); break;
        case 0x2026: au_buf_puts(out, "..."); break;
        case 0x00D7: au_buf_putc(out, 'x'); break;
        default:
            if (cp > 0xFFFF)
                au_buf_printf(out, "\\U%08lX", (unsigned long)cp);
            else
                au_buf_printf(out, "\\u%04lX", (unsigned long)cp);
            break;
        }
    }
}

/* ---- The sections ----
 * Each is built as UTF-8 in a scratch buffer, then appended as ASCII. */

static void heading(AuBuf *out, const char *title)
{
    if (out->len)
        au_buf_putc(out, '\n');
    au_buf_printf(out, "## %s\n\n", title);
}

static void fenced(AuBuf *out, AuBuf *body)
{
    au_buf_puts(out, "```\n");
    au_bugreport_ascii(body->data ? body->data : "", body->len, out);
    if (!body->len || body->data[body->len - 1] != '\n')
        au_buf_putc(out, '\n');
    au_buf_puts(out, "```\n");
    au_buf_clear(body);
}

static void details_open(AuBuf *out, const char *summary)
{
    /* The blank line after <summary> lets GitHub render the code block. */
    au_buf_printf(out, "<details>\n<summary>%s</summary>\n\n", summary);
}

static void details_close(AuBuf *out)
{
    au_buf_puts(out, "\n</details>\n");
}

static void put_redacted_kv(AuBuf *b, const char *label, const char *url)
{
    AuBuf u;
    au_buf_init(&u);
    au_bugreport_redact_url(url, &u);
    au_report_kv(b, label, "%s", au_buf_cstr(&u));
    au_buf_free(&u);
}

static const char *security_name(AuSecurity s)
{
    switch (s) {
    case AU_SECURITY_SECURE: return "secure (HTTPS, valid certificate)";
    case AU_SECURITY_NOT_SECURE: return "not secure (plain HTTP)";
    case AU_SECURITY_CERT_ERROR: return "certificate problem";
    case AU_SECURITY_MIXED: return "HTTPS with a warning (mixed content or no forward secrecy)";
    default: return "none (not a network page)";
    }
}

static void page_section(const AuBugReport *in, AuBuf *b)
{
    AuBrowserWindow *w = in->window;
    AuTab *t = w ? au_bw_active_tab(w) : NULL;
    if (!t) {
        au_buf_puts(b, "  No browser window was open.\n");
        return;
    }
    AuPage *p = t->page;
    AuPageStats st;
    au_page_stats(p, &st);
    put_redacted_kv(b, "Address", au_page_url(p));
    au_report_kv(b, "Title", "%s", au_page_title(p)[0] ? au_page_title(p) : "(none)");
    au_report_kv(b, "Loading", "%s", au_page_loading(p) ? "still loading when the report was made" : "finished");
    au_report_kv(b, "User agent", "%s", au_prefs_user_agent(&in->browser->prefs));
    au_report_kv(b, "Security", "%s", security_name(au_page_security(p)));
    const AuFetchCert *cert = au_page_cert(p);
    if (cert && cert->protocol[0]) {
        bool legacy = !strncmp(cert->cipher, "RSA-", 4);
        au_report_kv(b, "TLS", "%s, %s", cert->protocol, cert->cipher[0] ? cert->cipher : "cipher not recorded");
        au_report_kv(b, "Legacy key exchange", "%s", legacy ? "yes (RSA key exchange, no forward secrecy)" : "no");
        au_report_kv(b, "Certificate exception", "%s",
                     cert->overridden ? "yes (the person chose Continue Anyway)" : "no");
        if (cert->error && cert->reason[0])
            au_report_kv(b, "Certificate problem", "%s", cert->reason);
    } else {
        au_report_kv(b, "TLS", "none");
    }
    au_report_kv(b, "Mixed content", "%s",
                 au_page_security(p) == AU_SECURITY_MIXED && !(cert && !strncmp(cert->cipher, "RSA-", 4)) ? "yes" : "no");
    au_report_kv(b, "Blocked requests", "%lu", (unsigned long)st.blocked);
    if (in->other_tabs) {
        for (int i = 0; i < in->browser->nwindows; i++) {
            AuBrowserWindow *ow = in->browser->windows[i];
            for (int k = 0; k < ow->ntabs; k++) {
                if (ow->tabs[k] == t)
                    continue;
                char label[32];
                snprintf(label, sizeof label, "Tab %d.%d", i + 1, k + 1);
                put_redacted_kv(b, label, au_page_url(ow->tabs[k]->page));
            }
        }
    } else {
        au_report_kv(b, "Other tabs", "addresses not included");
    }
}

/* The page's errors and warnings since its load began, the last
 * AU_BUGREPORT_CONSOLE_LINES lines. Returns how many it wrote. */
static int console_section(AuPage *p, AuBuf *b)
{
    size_t n = au_page_console_count(p), cur = au_page_console_current(p);
    /* Count the lines first, so that the newest fit. */
    int total = 0;
    for (size_t i = n - cur; i < n; i++) {
        AuConsoleEntry e;
        if (!au_dev_console_entry(p, i, &e) || (e.level != 2 && e.level != 3))
            continue;
        total++;
        for (const char *s = e.text; (s = strchr(s, '\n')) != NULL; s++)
            total++;
    }
    int skip = total > AU_BUGREPORT_CONSOLE_LINES ? total - AU_BUGREPORT_CONSOLE_LINES : 0, at = 0, wrote = 0;
    AuBuf line, red;
    au_buf_init(&line);
    au_buf_init(&red);
    for (size_t i = n - cur; i < n; i++) {
        AuConsoleEntry e;
        if (!au_dev_console_entry(p, i, &e) || (e.level != 2 && e.level != 3))
            continue;
        const char *s = e.text;
        bool first = true;
        for (;;) {
            size_t k = strcspn(s, "\n");
            if (at++ >= skip) {
                au_buf_clear(&line);
                au_buf_clear(&red);
                if (first)
                    au_buf_printf(&line, "%-5s  ", au_dev_console_label(e.level));
                else
                    au_buf_puts(&line, "       ");
                au_buf_append(&line, s, k > 300 ? 300 : k);
                if (k > 300)
                    au_buf_puts(&line, "...");
                au_bugreport_redact_text(au_buf_cstr(&line), &red);
                au_buf_append(b, red.data, red.len);
                au_buf_putc(b, '\n');
                wrote++;
            }
            first = false;
            if (!s[k])
                break;
            s += k + 1;
        }
    }
    au_buf_free(&line);
    au_buf_free(&red);
    return wrote;
}

/* The preferences that change how pages load and look, where they
 * differ from the defaults. Never the home page, the search template,
 * the download folder, per-site lists or proxy addresses. */
static void settings_section(const AuBugReport *in, AuBuf *b)
{
    static const struct {
        const char *key, *label;
    } shown[] = {
        {"content.javascript", "JavaScript"},
        {"content.images", "Load images"},
        {"content.block_popups", "Block pop-up windows"},
        {"privacy.cookies", "Cookies"},
        {"privacy.block_third_party_cookies", "Block third-party cookies"},
        {"privacy.do_not_track", "Send Do Not Track"},
        {"privacy.global_privacy_control", "Send Global Privacy Control"},
        {"privacy.https_first", "HTTPS first"},
        {"security.legacy_tls", "Allow old servers (legacy TLS)"},
        {"privacy.block_trackers", "Content blocking"},
        {"blocking.lists", "Blocking lists"},
        {"blocking.update", "Blocking list updates"},
        {"cache.memory_kb", "Memory cache (KB)"},
        {"cache.disk_kb", "Disk cache (KB)"},
        {"cache.documents", "Kept documents per tab"},
        {"cache.documents_kb", "Kept documents (KB)"},
        {"network.user_agent", "User agent"},
        {"fonts.serif", "Serif font"},
        {"fonts.sans_serif", "Sans-serif font"},
        {"fonts.monospace", "Monospace font"},
        {"fonts.size", "Font size"},
        {"fonts.size_monospace", "Monospace font size"},
        {"fonts.minimum_size", "Minimum font size"},
    };
    const AuPrefs *p = &in->browser->prefs;
    AuPrefs def;
    au_prefs_init(&def);
    AuBuf cur, dv;
    au_buf_init(&cur);
    au_buf_init(&dv);
    int n = 0;
    for (size_t i = 0; i < sizeof shown / sizeof shown[0]; i++) {
        au_buf_clear(&cur);
        au_buf_clear(&dv);
        au_prefs_get(p, shown[i].key, &cur);
        au_prefs_get(&def, shown[i].key, &dv);
        if (!strcmp(au_buf_cstr(&cur), au_buf_cstr(&dv)))
            continue;
        au_buf_printf(b, "  %-32s %s   (default: %s)\n", shown[i].label, cur.len ? au_buf_cstr(&cur) : "(empty)",
                      dv.len ? au_buf_cstr(&dv) : "(empty)");
        n++;
    }
    /* Shown as set or as a count only. */
    if (p->http_proxy_host[0] || p->https_proxy_host[0]) {
        au_buf_printf(b, "  %-32s HTTP %s, HTTPS %s   (default: not set)\n", "Proxies",
                      p->http_proxy_host[0] ? "set" : "not set", p->https_proxy_host[0] ? "set" : "not set");
        n++;
    }
    static const char *const counted[][2] = {
        {"blocking.custom", "Custom blocking lists"},
        {"blocking.allow", "Domains always allowed"},
        {"blocking.block", "Domains always blocked"},
        {"blocking.sites_off", "Sites with blocking off"},
    };
    for (size_t i = 0; i < sizeof counted / sizeof counted[0]; i++) {
        au_buf_clear(&cur);
        au_prefs_get(p, counted[i][0], &cur);
        int words = 0;
        bool in_word = false;
        for (size_t k = 0; k < cur.len; k++) {
            bool sp = cur.data[k] == ' ' || cur.data[k] == '\n' || cur.data[k] == '\t';
            if (!sp && !in_word)
                words++;
            in_word = !sp;
        }
        if (!words)
            continue;
        au_buf_printf(b, "  %-32s %d   (default: 0; the entries are not included)\n", counted[i][1], words);
        n++;
    }
    if (!n)
        au_buf_puts(b, "  None: every setting that changes how pages load is at its default.\n");
    bool off = false;
    if (in->window && au_bw_site_blocking(in->window, &off))
        au_buf_printf(b, "  %-32s %s\n", "Blocking on this page's site", off ? "turned off for this site" : "on");
    au_buf_free(&cur);
    au_buf_free(&dv);
    au_prefs_free(&def);
}

void au_bugreport_build(const AuBugReport *in, AuBuf *out)
{
    AuBuf b, red;
    au_buf_init(&b);
    au_buf_init(&red);
    AuTab *t = in->window ? au_bw_active_tab(in->window) : NULL;
    AuPage *p = t ? t->page : NULL;
    AuSysInfo none;
    const AuSysInfo *sys = in->sys;
    if (!sys) {
        memset(&none, 0, sizeof none);
        none.virtual_memory = none.enforcer = -1;
        sys = &none;
    }

    /* 1. For the person to fill in. */
    au_buf_puts(out, "<!-- Made by Aura (Help > Report a Problem). Fill in the first three sections; "
                     "you can edit or remove anything below them. -->\n");
    heading(out, "What I did");
    au_buf_puts(out, "(the steps, one per line)\n");
    heading(out, "What I expected");
    au_buf_puts(out, "(what should have happened)\n");
    heading(out, "What happened");
    au_buf_puts(out, "(what you saw; a screenshot or a photo of the screen helps)\n");

    /* 2. The version. */
    heading(out, "Aura");
    au_report_kv(&b, "Version", "%s", au_build_identity());
    au_report_kv(&b, "Built from", "the commit of %s", au_build_date());
    fenced(out, &b);

    /* 3. The machine. */
    heading(out, "Machine");
    au_sysinfo_machine(sys, &b);
    if (sys->enforcer < 0)
        au_buf_puts(&b, "  (Enforcer: please say whether it is installed)\n");
    fenced(out, &b);

    /* 4. Aura's state. */
    heading(out, "Aura's state");
    au_sysinfo_state(sys, in->browser, in->host, &b);
    fenced(out, &b);

    /* 5. The active page. */
    heading(out, "Page");
    page_section(in, &b);
    fenced(out, &b);

    /* 6. Its console. */
    heading(out, "Console");
    if (p) {
        int wrote = console_section(p, &b);
        if (!wrote)
            au_buf_puts(&b, "No errors or warnings.\n");
    } else {
        au_buf_puts(&b, "No page.\n");
    }
    au_buf_printf(out, "Errors and warnings of this page, the last %d lines at most.\n\n", AU_BUGREPORT_CONSOLE_LINES);
    fenced(out, &b);

    /* 7. The request log: problems and totals, then the table. */
    heading(out, "Network");
    if (p) {
        const AuNetLog *l = au_page_netlog(p);
        au_netlog_summary(l, NULL, &red);
        au_bugreport_redact_text(au_buf_cstr(&red), &b);
        au_buf_clear(&red);
        fenced(out, &b);
        char title[96];
        uint32_t first = l->count > AU_BUGREPORT_REQUESTS ? l->count - AU_BUGREPORT_REQUESTS : 0;
        snprintf(title, sizeof title, "The request table (%lu of %lu requests)", (unsigned long)(l->count - first),
                 (unsigned long)l->requests);
        details_open(out, title);
        au_netlog_header(&b);
        au_buf_putc(&b, '\n');
        for (uint32_t i = first; i < l->count; i++) {
            const AuNetRecord *r = au_netlog_at(l, i);
            au_buf_clear(&red);
            au_bugreport_redact_url(r->url, &red);
            au_netlog_row_url(r, au_buf_cstr(&red), 0, &b);
            au_buf_putc(&b, '\n');
        }
        au_buf_clear(&red);
        fenced(out, &b);
        details_close(out);
    } else {
        au_buf_puts(&b, "No page.\n");
        fenced(out, &b);
    }

    /* 8. Performance. */
    heading(out, "Performance");
    details_open(out, "The Performance report");
    if (p) {
        AuDevHostInfo h;
        char machine[256];
        if (in->host)
            h = *in->host;
        else
            memset(&h, 0, sizeof h);
        au_sysinfo_line(sys, machine, sizeof machine);
        h.machine = machine;
        h.build = au_build_identity();
        h.connection_text = NULL; /* host names of other pages' connections stay out */
        au_dev_perf_report(p, &h, in->browser->host.time_ms ? in->browser->host.time_ms(in->browser->host.ctx) : 0, &red);
        au_bugreport_redact_text(au_buf_cstr(&red), &b);
        au_buf_clear(&red);
    } else {
        au_buf_puts(&b, "No page.\n");
    }
    fenced(out, &b);
    details_close(out);

    /* 9. Settings. */
    heading(out, "Settings that differ from the defaults");
    settings_section(in, &b);
    fenced(out, &b);

    /* 10. The clock. */
    heading(out, "Clock");
    au_sysinfo_clock(sys, &b);
    fenced(out, &b);

    au_buf_puts(out, "\n---\n");
    au_buf_printf(out,
                  "Left out by Aura: cookies, request and response headers, request bodies and form data, history, "
                  "bookmarks, the home page and paths on the disk. In addresses, user names and passwords are "
                  "removed, a query string or fragment longer than %d characters is cut there with \"...\", and a "
                  "file: address keeps only its file name.%s\n",
                  AU_BUGREPORT_QUERY_MAX,
                  in->other_tabs ? "" : " The addresses of other tabs are not included.");
    au_buf_free(&b);
    au_buf_free(&red);
}
