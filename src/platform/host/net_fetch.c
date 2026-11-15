/* aura-host fetch: one HTTP(S) request through the engine's network stack.
 *
 *   aura-host fetch URL [-o file] [-v] [-X METHOD] [-d data] [-H "Name: value"]...
 *                       [-u user:pass] [--referer URL] [--https-first]
 *                       [--allow-http] [--no-cookies] [--max-time S]
 *
 * The body goes to stdout (or -o file). -v prints the request outcome,
 * response headers, TLS details and timings on stderr. Cookies and HSTS
 * state persist between runs in the profile folder ($AURA_PROFILE, else
 * ~/.aura-host): files "cookies" and "hsts", both in the engine's text
 * formats. Exit status: 0 for a 2xx/3xx response, 1 for 4xx/5xx, 2 for
 * usage, 3 for a network error. */
#include "net/cookies.h"
#include "net/hsts.h"
#include "net/roots.h"
#include "net/tls.h"
#include "net_host.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Fetch {
    FILE *out;
    bool verbose;
    bool done;
    AuNetError err;
    int status;
    int64_t bytes;
    const char *user, *pass;
    AuCertInfo cert;
    bool have_cert;
    double t_start, t_response;
    AuHostNet *host;
} Fetch;

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int usage(void)
{
    fprintf(stderr, "usage: aura-host fetch URL [-o file] [-v] [-X METHOD] [-d data] [-H \"Name: value\"]...\n"
                    "                       [-u user:pass] [--referer URL] [--https-first] [--allow-http]\n"
                    "                       [--no-cookies] [--max-time S]\n");
    return 2;
}

static void fmt_time(int64_t t, char *out, size_t cap)
{
    if (!t) {
        snprintf(out, cap, "?");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

static void print_cert(const AuCertInfo *ci)
{
    char a[40], b[40];
    bool bad = ci->error != 0;
    fprintf(stderr, "* %s for %s%s%s\n", bad ? "TLS/certificate failure" : "certificate valid", ci->host,
            bad && ci->reason ? ": " : "", bad && ci->reason ? ci->reason : "");
    if (ci->clock_wrong)
        fprintf(stderr, "* the system clock is wrong (before the root store's date)\n");
    if (ci->hsts)
        fprintf(stderr, "* HSTS host: the error cannot be overridden\n");
    if (ci->added)
        fprintf(stderr, "* the server's chain was incomplete: the last %d certificate(s) came from caIssuers URLs\n",
                ci->added);
    if (bad && ci->missing_issuer)
        fprintf(stderr, "* the chain stops for want of an issuer; caIssuers URL: %s\n",
                ci->aia_url[0] ? ci->aia_url : "(none)");
    int n = ci->count < AU_CERT_MAX_CHAIN ? ci->count : AU_CERT_MAX_CHAIN;
    for (int i = 0; i < n; i++) {
        const AuCertEntry *e = &ci->chain[i];
        fmt_time(e->not_before, a, sizeof a);
        fmt_time(e->not_after, b, sizeof b);
        fprintf(stderr, "*  [%d] subject: %s\n*      issuer:  %s\n*      valid:   %s to %s\n", i, e->subject,
                e->issuer, a, b);
        if (e->san[0])
            fprintf(stderr, "*      names:   %s\n", e->san);
        fprintf(stderr, "*      sha256:  ");
        for (int k = 0; k < 32; k++)
            fprintf(stderr, "%02X%s", e->sha256[k], k < 31 ? ":" : "\n");
    }
}

static void on_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    Fetch *f = ctx;
    f->status = r->status;
    f->t_response = mono_ms();
    const AuCertInfo *ci = au_http_cert_info(t);
    if (ci) {
        f->cert = *ci;
        f->have_cert = true;
    }
    if (!f->verbose)
        return;
    fprintf(stderr, "< %d %s  (%s, %d redirect%s, %.0f ms)\n", r->status, r->reason, r->url, r->redirects,
            r->redirects == 1 ? "" : "s", f->t_response - f->t_start);
    for (size_t i = 0; i < r->headers->n; i++)
        fprintf(stderr, "< %s: %s\n", r->headers->items[i].name, r->headers->items[i].value);
    if (r->security.tls)
        fprintf(stderr, "* %s, %s, ALPN %s%s\n", au_tls_version_name(r->security.version),
                r->security.cipher ? r->security.cipher : "?",
                r->security.alpn && r->security.alpn[0] ? r->security.alpn : "none",
                r->security.resumed ? ", resumed" : "");
}

static void on_data(void *ctx, AuHttpTransfer *t, const void *data, size_t len)
{
    (void)t;
    Fetch *f = ctx;
    fwrite(data, 1, len, f->out);
    f->bytes += (int64_t)len;
}

static void on_done(void *ctx, AuHttpTransfer *t, AuNetError err)
{
    Fetch *f = ctx;
    f->done = true;
    f->err = err;
    if (err == AU_NET_ERR_CERT || err == AU_NET_ERR_CLOCK || err == AU_NET_ERR_TLS ||
        err == AU_NET_ERR_HTTPS_FALLBACK) {
        const AuCertInfo *ci = au_http_cert_info(t);
        if (ci) {
            f->cert = *ci;
            f->have_cert = true;
        }
    }
}

static bool on_redirect(void *ctx, AuHttpTransfer *t, int status, const char *url)
{
    (void)t;
    Fetch *f = ctx;
    if (f->verbose)
        fprintf(stderr, "* %d redirect to %s\n", status, url);
    return true;
}

static int on_credentials(void *ctx, AuHttpTransfer *t, const AuAuthChallenge *ch, AuBuf *user, AuBuf *pass)
{
    (void)t;
    Fetch *f = ctx;
    if (f->verbose)
        fprintf(stderr, "* %s %s authentication for realm \"%s\" at %s (attempt %d)\n", ch->proxy ? "proxy" : "server",
                ch->scheme, ch->realm, ch->host, ch->attempt);
    if (!f->user || ch->attempt > 1)
        return 0;
    au_buf_puts(user, f->user);
    au_buf_puts(pass, f->pass);
    return 1;
}

int au_host_fetch_main(int argc, char **argv)
{
    const char *url = NULL, *outpath = NULL, *method = NULL, *data = NULL, *referer = NULL, *userpass = NULL;
    bool verbose = false, https_first = false, allow_http = false, cookies = true;
    double max_time = 0;
    AuHttpRequest rq;
    AuHeaders extra;
    au_headers_init(&extra);
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)
            outpath = argv[++i];
        else if (!strcmp(a, "-v"))
            verbose = true;
        else if (!strcmp(a, "-X") && i + 1 < argc)
            method = argv[++i];
        else if (!strcmp(a, "-d") && i + 1 < argc)
            data = argv[++i];
        else if (!strcmp(a, "-u") && i + 1 < argc)
            userpass = argv[++i];
        else if (!strcmp(a, "--referer") && i + 1 < argc)
            referer = argv[++i];
        else if (!strcmp(a, "--https-first"))
            https_first = true;
        else if (!strcmp(a, "--allow-http"))
            allow_http = true;
        else if (!strcmp(a, "--no-cookies"))
            cookies = false;
        else if (!strcmp(a, "--max-time") && i + 1 < argc)
            max_time = atof(argv[++i]);
        else if (!strcmp(a, "-H") && i + 1 < argc) {
            const char *h = argv[++i], *colon = strchr(h, ':');
            if (!colon || colon == h) {
                au_headers_free(&extra);
                return usage();
            }
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            au_headers_addn(&extra, h, (size_t)(colon - h), v, strlen(v));
        } else if (a[0] != '-' && !url)
            url = a;
        else {
            au_headers_free(&extra);
            return usage();
        }
    }
    if (!url) {
        au_headers_free(&extra);
        return usage();
    }

    Fetch f;
    memset(&f, 0, sizeof f);
    f.verbose = verbose;
    f.out = stdout;
    if (outpath) {
        f.out = fopen(outpath, "wb");
        if (!f.out) {
            fprintf(stderr, "aura-host: cannot write %s\n", outpath);
            au_headers_free(&extra);
            return 2;
        }
    }
    char user[256] = "", *pass = NULL;
    if (userpass) {
        snprintf(user, sizeof user, "%s", userpass);
        pass = strchr(user, ':');
        if (pass)
            *pass++ = 0;
        f.user = user;
        f.pass = pass ? pass : "";
    }

    AuHostSession ses;
    if (!au_host_session_open(&ses, cookies, verbose)) {
        fprintf(stderr, "aura-host: cannot set up sockets\n");
        return 3;
    }
    AuHostNet *host = ses.host;
    AuNet *net = ses.net;
    f.host = host;
    au_net_config(net)->https_first = https_first;

    au_http_request_init(&rq, url);
    rq.headers = extra;
    rq.referrer = referer;
    rq.user_activated = true;
    rq.allow_http_fallback = allow_http;
    if (max_time > 0)
        rq.total_timeout_ms = (uint32_t)(max_time * 1000);
    if (data) {
        rq.method = method ? method : "POST";
        rq.body = au_body_from_bytes(data, strlen(data),
                                     au_headers_get(&rq.headers, "content-type")
                                         ? NULL
                                         : "application/x-www-form-urlencoded");
    } else if (method) {
        rq.method = method;
    }
    AuHttpCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &f;
    cb.response = on_response;
    cb.data = on_data;
    cb.done = on_done;
    cb.redirect = on_redirect;
    cb.credentials = on_credentials;
    if (verbose)
        fprintf(stderr, "> %s %s\n", rq.method, url);
    f.t_start = mono_ms();
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    while (!f.done) {
        if (!au_net_pump(net))
            au_host_net_wait(host, 20);
    }
    double t_end = mono_ms();

    if (verbose && f.have_cert)
        print_cert(&f.cert);
    int rc = 0;
    if (f.err != AU_NET_OK) {
        fprintf(stderr, "aura-host: %s: %s\n", url, au_net_error_name(f.err));
        if (!verbose && f.have_cert)
            print_cert(&f.cert);
        if (f.err == AU_NET_ERR_HTTPS_FALLBACK)
            fprintf(stderr, "aura-host: https failed; run with --allow-http to load over http\n");
        rc = 3;
    } else if (f.status >= 400) {
        rc = 1;
    }
    if (verbose)
        fprintf(stderr, "* %lld body bytes, %.0f ms in all\n", (long long)f.bytes, t_end - f.t_start);

    au_host_session_close(&ses); /* saves the cookie and HSTS files */
    if (outpath)
        fclose(f.out);
    return rc;
}
