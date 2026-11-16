/* Runs pages made by Domato (Google Project Zero's grammar-based DOM
 * fuzzer; see tests/domato) through the document view with script on.
 * Each page is fed in pieces, its requests are refused, and then its
 * scripts, timers and animation frames run in steps with a forced
 * layout, a paint, hit tests, mouse and key events between them, at two
 * viewport sizes; the view is freed with script objects alive. The
 * random choices (piece sizes, points, keys) come from SEED and the
 * page's number.
 *
 * Run it under AddressSanitizer and UBSan. A page whose steps take more
 * than AURA_FUZZ_SLOW_MS (default 120000) of CPU time aborts: script
 * that does not end must be stopped by its time limit.
 *
 * usage: fuzz_domato SECONDS SEED PAGE.html...
 *        fuzz_domato replay PAGE.html... */
#include "../fuzz.h"
#include "../layout/ahem.h"
#include "page/page.h"

static AuFontSet *fonts;
static AuSurface *surface;
static long slow_ms = 120000;
static double fake_now;

/* CPU time, twenty times as fast: the engine's ten-second limit on a
 * script costs half a second here. */
static double now_ms(void *ctx, AuDocView *dv)
{
    (void)ctx, (void)dv;
    fake_now += 0.001;
    return fake_now + (double)clock() * 20000.0 / CLOCKS_PER_SEC;
}

typedef struct Loader {
    uint32_t q[64];
    int n;
    uint32_t next;
} Loader;

static uint32_t req_fn(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    Loader *l = (Loader *)ctx;
    (void)dv, (void)url, (void)kind;
    if (l->n == 64 || l->next >= 200)
        return 0;
    l->q[l->n++] = ++l->next;
    return l->next;
}

static void answer(Loader *l, AuDocView *dv)
{
    /* Every resource fails or is empty: the pages name files Domato
     * does not make. */
    while (l->n) {
        uint32_t id = l->q[--l->n];
        au_docview_resource_done(dv, id, fz_rnd(2) != 0);
    }
}

static void step(AuDocView *dv, Loader *l, int w, int h)
{
    AuCanvas *c;
    AuHitResult hr;
    AuEvent ev;
    int cw, ch, m;
    answer(l, dv);
    for (m = 0; m < 4; m++)
        au_docview_run_scripts(dv, 5);
    au_docview_update(dv);
    au_docview_content_size(dv, &cw, &ch);
    c = au_canvas_new(surface);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 150}, 0, ch > h ? (int)fz_rnd((uint32_t)(ch - h)) : 0);
    au_canvas_free(c);
    for (m = 0; m < 3; m++) {
        memset(&ev, 0, sizeof ev);
        ev.type = m == 0 ? AU_EVENT_MOUSE_MOVE : m == 1 ? AU_EVENT_MOUSE_DOWN : AU_EVENT_MOUSE_UP;
        if (m != 2) {
            ev.x = (int)fz_rnd((uint32_t)w);
            ev.y = (int)fz_rnd((uint32_t)h);
        }
        ev.clicks = 1;
        au_docview_mouse(dv, &ev);
        au_docview_hit_test(dv, ev.x, ev.y, &hr);
    }
    memset(&ev, 0, sizeof ev);
    ev.type = AU_EVENT_KEY_DOWN;
    if (fz_rnd(2))
        ev.key = fz_rnd(2) ? AU_KEY_TAB : AU_KEY_RETURN;
    else
        ev.ch = 'a' + fz_rnd(26);
    au_docview_key(dv, &ev);
}

static void run_one(const FzBuf *in)
{
    Loader l;
    AuLoader loader;
    AuDocViewCallbacks cb;
    AuDocView *dv;
    size_t pos = 0;
    int k;
    memset(&l, 0, sizeof l);
    memset(&loader, 0, sizeof loader);
    memset(&cb, 0, sizeof cb);
    loader.ctx = &l;
    loader.request = req_fn;
    cb.now_ms = now_ms;
    fake_now = 1000;
    dv = au_docview_new(fonts, &loader, &cb);
    au_docview_set_scripting(dv, true);
    au_docview_set_viewport(dv, 640, 480);
    au_docview_begin(dv, "http://domato.example/page.html");
    while (pos < in->n) {
        size_t chunk = 1 + fz_rnd(fz_rnd(3) ? 2000 : 60000);
        if (chunk > in->n - pos)
            chunk = in->n - pos;
        au_docview_feed(dv, (const char *)in->d + pos, chunk);
        pos += chunk;
        if (fz_rnd(4) == 0) {
            answer(&l, dv);
            au_docview_run_scripts(dv, 5);
            au_docview_update(dv);
        }
    }
    au_docview_finish(dv);
    for (k = 0; k < 6; k++)
        step(dv, &l, 640, 480);
    au_docview_set_viewport(dv, 1 + (int)fz_rnd(1000), 1 + (int)fz_rnd(700));
    for (k = 0; k < 3; k++)
        step(dv, &l, 300, 200);
    au_docview_free(dv);
}

static int timed(const FzBuf *in, const char *name)
{
    clock_t t0 = clock();
    long ms;
    run_one(in);
    ms = (long)((double)(clock() - t0) * 1000 / CLOCKS_PER_SEC);
    if (ms > slow_ms) {
        printf("fuzz_domato: %s took %ld ms\n", name, ms);
        abort();
    }
    return 0;
}

static int fuzz_main(int argc, char **argv)
{
    static uint8_t fontdata[16384];
    FzBuf in = {0};
    int i, replay, done = 0;
    if (argc < 3) {
        printf("usage: fuzz_domato SECONDS SEED PAGE.html...\n       fuzz_domato replay PAGE.html...\n");
        return 2;
    }
    replay = !strcmp(argv[1], "replay");
    if (getenv("AURA_FUZZ_SLOW_MS"))
        slow_ms = atol(getenv("AURA_FUZZ_SLOW_MS"));
    fz_base_seed = replay ? 1 : (uint32_t)strtoul(argv[2], NULL, 10);
    fz_cap(replay ? 0 : atol(argv[1]));
    fonts = ahem_fontset(fontdata);
    surface = au_surface_new(200, 150);
    for (i = replay ? 2 : 3; i < argc && !fz_expired(); i++) {
        if (!fz_read_file(argv[i], &in, 4u << 20))
            continue;
        /* The page's number in the list seeds its choices; a replayed
         * page takes the state the run saved, when there is one. */
        fz_iteration(i - 3);
        if (replay)
            fz_replay_seed(argv[i]);
        else if (getenv("AURA_FUZZ_VERBOSE")) {
            printf("%s\n", argv[i]);
            fflush(stdout);
        }
        {
            /* Leave the state beside the page for replay. */
            char sp[1100];
            FILE *f;
            if (!replay && strlen(argv[i]) < 1000) {
                snprintf(sp, sizeof sp, "%s.seed", argv[i]);
                f = fopen(sp, "w");
                if (f) {
                    fprintf(f, "%lu\n", (unsigned long)fz_state);
                    fclose(f);
                }
            }
        }
        timed(&in, argv[i]);
        free(in.d);
        memset(&in, 0, sizeof in);
        done++;
    }
    printf("fuzz_domato: %d pages\n", done);
    au_surface_free(surface);
    au_fontset_free(fonts);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
