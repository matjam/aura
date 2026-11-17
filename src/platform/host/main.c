/* aura-host: the headless development tool.
 *
 *   aura-host scene [-o out.png] [-w width] [-h height] [-s scroll]
 *                   [--font file.ttf]... [--no-system-fonts]
 *       Draws the paint test scene and writes it as a PNG.
 *   aura-host fonts [--font file.ttf]...
 *       Lists the registered font faces and the generic families.
 *   aura-host font-index DIR [-o FILE]
 *       Writes the index of a Fonts folder (DIR/fonts.idx).
 *   aura-host bench [--font file.ttf]...
 *       Times the rasterizer, compositing and text.
 *   aura-host chrome [-o out.png] [-w W] [-h H] [--tabs top|left] [--tab URL]...
 *       Renders a browser window (chrome and stub page) as a PNG. See
 *       usage() for the options. */
#include "browser/scriptmem.h"
#include "core/region.h"
#include "host.h"
#include "png.h"
#include "browser/browser.h"
#include "browser/present.h"
#include "browser/scene.h"
#include "browser/url.h"
#include "render.h"
#include "net_host.h"
#include "net/blocklist.h"
#include "net/cache.h"
#include "net/loader.h"
#include "net/net_prefs.h"
#include "net/web_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- Memory report (builds with AURA_MEM_TRACK) ----
 * Prints the bytes live per source folder and per file, largest first,
 * with what each held when the total was at its peak. */
void au_host_mem_report(const char *label)
{
#ifdef AU_MEM_TRACK
    static AuMemSite sites[4096];
    int n = au_mem_sites(sites, 4096);
    struct {
        char name[48];
        size_t live, at_peak, blocks;
    } dirs[64];
    int nd = 0;
    for (int i = 0; i < n; i++) {
        const char *f = sites[i].file;
        for (const char *q; (q = strstr(f + 1, "/src/")) != NULL;)
            f = q;
        if (!strncmp(f, "/src/", 5))
            f++;
        sites[i].file = f;
        const char *e = strrchr(f, '/');
        size_t len = e ? (size_t)(e - f) : strlen(f);
        if (len > 47)
            len = 47;
        int d;
        for (d = 0; d < nd; d++)
            if (strlen(dirs[d].name) == len && !memcmp(dirs[d].name, f, len))
                break;
        if (d == nd) {
            if (nd == 64)
                continue;
            memset(&dirs[nd], 0, sizeof dirs[nd]);
            memcpy(dirs[nd].name, f, len);
            nd++;
        }
        dirs[d].live += sites[i].live;
        dirs[d].at_peak += sites[i].at_peak;
        dirs[d].blocks += sites[i].blocks;
    }
    fprintf(stderr, "memory (%s): %lu KB live, %lu KB at the peak\n", label, (unsigned long)(au_mem_live() / 1024),
            (unsigned long)(au_mem_peak() / 1024));
    fprintf(stderr, "  %-28s %10s %10s %9s\n", "folder", "live KB", "at peak", "blocks");
    for (;;) {
        int best = -1;
        for (int d = 0; d < nd; d++)
            if (dirs[d].name[0] && (best < 0 || dirs[d].live > dirs[best].live))
                best = d;
        if (best < 0)
            break;
        fprintf(stderr, "  %-28s %10.1f %10.1f %9lu\n", dirs[best].name, dirs[best].live / 1024.0,
                dirs[best].at_peak / 1024.0, (unsigned long)dirs[best].blocks);
        dirs[best].name[0] = 0;
    }
    fprintf(stderr, "  %-28s %10s %10s %9s %10s\n", "file:line", "live KB", "at peak", "blocks", "own peak");
    for (;;) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (sites[i].file && (best < 0 || sites[i].live + sites[i].at_peak > sites[best].live + sites[best].at_peak))
                best = i;
        if (best < 0 || sites[best].live + sites[best].at_peak < (size_t)(getenv("AURA_MEM_MIN") ? atoi(getenv("AURA_MEM_MIN")) : 65536))
            break;
        fprintf(stderr, "  %-28s %10.1f %10.1f %9lu %10.1f\n", sites[best].file, sites[best].live / 1024.0,
                sites[best].at_peak / 1024.0, (unsigned long)sites[best].blocks, sites[best].peak / 1024.0);
        sites[best].file = NULL;
    }
#else
    (void)label;
    fprintf(stderr, "aura-host: memory report needs a build with -DAURA_MEM_TRACK=ON\n");
#endif
}

/* ---- The pooled allocator on the host (AURA_HEAP=1) ----
 * Runs au_alloc through the pool the Mac OS 9 application uses
 * (src/core/heap.c), over malloc, so a page exercises it here and
 * --do heap can show what it holds. */
static void *host_heap_get(size_t n, unsigned how)
{
    (void)how;
    return malloc(n);
}

static void host_heap_release(void *p)
{
    free(p);
}

/* ---- A stand-in for the Mac's application heap (AURA_HEAP=mac[:MB][:MODE]) ----
 * The pool's platform blocks come from one region of MB megabytes
 * (default 160; the host's structures are larger than the Mac's) with
 * the Memory Manager's placement: NewPtr takes the lowest free space that
 * fits. Where the long-lived zone goes is MODE:
 *   (none)  outside the region, as the Mac application keeps it in
 *           temporary memory (heap_get in src/platform/macos9/main.c)
 *   top     at the top of the region, highest free space first
 *   flat    low with the page zone, as before the zones were placed
 * A block the region has no room for goes outside it too, as the Mac
 * takes temporary memory when its heap is short. --do heapmap then shows
 * the free space, the largest free block, and which blocks sit where, so
 * the blocks that split the free space can be found. Each block's tag
 * holds its allocation number and how. */
static AuRegion sim_heap;
static unsigned long sim_serial;
static size_t sim_high;     /* the highest end of a page block so far, from the region's start */
static size_t sim_out_long; /* long-lived bytes outside the region */
static size_t sim_out_page; /* page bytes outside the region (it had no room) */
static enum { SIM_OUT, SIM_TOP, SIM_FLAT } sim_mode;

typedef struct SimOut {
    size_t n, lng; /* two words: the block after it keeps malloc's alignment */
} SimOut;

static void *sim_heap_get(size_t n, unsigned how)
{
    bool lng = (how & AU_HEAP_ZONE_MASK) == AU_HEAP_ZONE_LONG;
    void *p = NULL;
    if (!lng || sim_mode != SIM_OUT)
        p = au_region_alloc(&sim_heap, n, lng && sim_mode == SIM_TOP);
    if (p) {
        *au_region_tag(p) = ((size_t)++sim_serial << 8) | (how & 0xFF);
        size_t end = (size_t)((char *)p - sim_heap.chunk[0].base) + au_region_size(p);
        if (!(lng && sim_mode == SIM_TOP) && end > sim_high)
            sim_high = end;
        return p;
    }
    SimOut *o = malloc(sizeof(SimOut) + n);
    if (!o)
        return NULL;
    o->n = n;
    o->lng = lng;
    *(lng ? &sim_out_long : &sim_out_page) += n;
    return o + 1;
}

static void sim_heap_release(void *p)
{
    if (au_region_owns(&sim_heap, p)) {
        au_region_free(&sim_heap, p);
        return;
    }
    SimOut *o = (SimOut *)p - 1;
    *(o->lng ? &sim_out_long : &sim_out_page) -= o->n;
    free(o);
}

static bool sim_heap_start(const char *spec)
{
    if (strncmp(spec, "mac", 3))
        return false;
    size_t mb = spec[3] == ':' ? (size_t)atol(spec + 4) : 160;
    if (mb < 8)
        mb = 8;
    sim_mode = strstr(spec, ":flat") ? SIM_FLAT : strstr(spec, ":top") ? SIM_TOP : SIM_OUT;
    void *mem = malloc(mb * 1024 * 1024);
    if (!mem)
        return false;
    au_region_init(&sim_heap);
    au_region_add(&sim_heap, mem, mb * 1024 * 1024);
    au_heap_enable(sim_heap_get, sim_heap_release);
    return true;
}

#ifdef AU_MEM_TRACK
#define SIM_SITES 48
typedef struct SimSites {
    struct {
        const char *site;
        size_t bytes, count;
    } s[SIM_SITES];
    int n;
    size_t bytes, count;
    void *last; /* the last block seen, and its size (the counter's header excluded) */
    size_t last_n;
} SimSites;

static void sim_site_add(SimSites *ss, const char *site, size_t bytes, size_t count)
{
    ss->bytes += bytes;
    ss->count += count;
    for (int i = 0; i < ss->n; i++)
        if (!strcmp(ss->s[i].site, site)) {
            ss->s[i].bytes += bytes;
            ss->s[i].count += count;
            return;
        }
    if (ss->n == SIM_SITES)
        return;
    ss->s[ss->n].site = site;
    ss->s[ss->n].bytes = bytes;
    ss->s[ss->n].count = count;
    ss->n++;
}

static void sim_block_site(void *ctx, void *block, size_t n)
{
    SimSites *ss = ctx;
    sim_site_add(ss, au_mem_site_of(block), n, 1);
    ss->last = block;
    ss->last_n = n;
}
#endif

typedef struct SimMap {
    FILE *out;
    char *base;
    size_t live_bytes, live_blocks, free_bytes;
    size_t long_low, long_bytes; /* the lowest long-lived block, and what the zone holds */
    unsigned long since;         /* blocks numbered after this are "new" */
#ifdef AU_MEM_TRACK
    SimSites pin; /* platform bytes per site that holds a block's most bytes */
#endif
} SimMap;

static void sim_map_block(void *ctx, void *p, size_t n, bool is_free)
{
    SimMap *m = ctx;
    size_t off = (size_t)((char *)p - m->base);
    if (is_free) {
        m->free_bytes += n;
        if (n >= 64 * 1024)
            fprintf(m->out, "%9.2f MB  free %8lu KB\n", off / 1048576.0, (unsigned long)(n / 1024));
        return;
    }
    size_t tag = *au_region_tag(p);
    unsigned how = (unsigned)(tag & 0xFF);
    unsigned long serial = (unsigned long)(tag >> 8);
    m->live_bytes += n;
    m->live_blocks++;
    bool lng = (how & AU_HEAP_ZONE_MASK) == AU_HEAP_ZONE_LONG;
    if (lng && off < m->long_low)
        m->long_low = off;
    if (lng)
        m->long_bytes += n;
    fprintf(m->out, "%9.2f MB  %s %-5s %6lu KB  #%lu%s", off / 1048576.0, lng ? "long" : "page",
            how & AU_HEAP_GET_SLAB ? "slab" : "large", (unsigned long)((n + 1023) / 1024), serial,
            serial > m->since ? " new" : "");
#ifdef AU_MEM_TRACK
    SimSites ss;
    memset(&ss, 0, sizeof ss);
    au_heap_walk_platform_block(p, how, sim_block_site, &ss);
    int best = -1;
    for (int k = 0; k < 3 && k < ss.n; k++) {
        int top = -1;
        for (int i = 0; i < ss.n; i++)
            if (ss.s[i].count && (top < 0 || ss.s[i].bytes > ss.s[top].bytes))
                top = i;
        if (top < 0)
            break;
        if (best < 0) {
            best = top;
            sim_site_add(&m->pin, ss.s[top].site, n, 1);
        }
        fprintf(m->out, "  %s %lu x %lu B", ss.s[top].site, (unsigned long)ss.s[top].count,
                (unsigned long)ss.s[top].bytes);
        ss.s[top].count = 0;
    }
    fprintf(m->out, "  (%lu blocks, %lu B)", (unsigned long)ss.count, (unsigned long)ss.bytes);
    if (ss.count == 1 && ss.last_n > 16 && ss.last_n <= 96) {
        /* a lone small block: its first bytes, after the counter's header */
        const unsigned char *t = (const unsigned char *)ss.last + 16;
        fputs("  \"", m->out);
        for (size_t i = 0; i + 16 < ss.last_n && i < 48; i++)
            fputc(t[i] >= 32 && t[i] < 127 ? t[i] : '.', m->out);
        fputc('"', m->out);
    }
#endif
    fputc('\n', m->out);
}

/* --do heapmap[:FILE]: trims the pool (as the Mac does when idle),
 * prints the free space and the largest free block, and writes the map to
 * FILE (or stdout). Blocks numbered after the previous heapmap are
 * marked new. */
static void sim_heap_map(const char *file)
{
    static unsigned long last;
    if (!sim_heap.nchunks) {
        printf("heapmap: needs AURA_HEAP=mac\n");
        return;
    }
    au_heap_trim();
    size_t freeb, largest;
    au_region_free_space(&sim_heap, &freeb, &largest);
    printf("heapmap: %lu KB free of %lu KB, largest free block %lu KB, %lu platform blocks (%lu KB); "
           "page blocks reached %lu KB; outside: %lu KB long-lived, %lu KB page; block #%lu\n",
           (unsigned long)(freeb / 1024), (unsigned long)(sim_heap.total / 1024), (unsigned long)(largest / 1024),
           (unsigned long)sim_heap.blocks, (unsigned long)(sim_heap.used / 1024), (unsigned long)(sim_high / 1024),
           (unsigned long)(sim_out_long / 1024), (unsigned long)(sim_out_page / 1024), sim_serial);
    SimMap m;
    memset(&m, 0, sizeof m);
    m.out = file ? fopen(file, "w") : NULL;
    if (!m.out)
        m.out = stdout;
    m.base = sim_heap.chunk[0].base;
    m.since = last;
    m.long_low = sim_heap.total;
    au_region_walk(&sim_heap, sim_map_block, &m);
    if (m.long_bytes)
        printf("heapmap: long-lived zone in the region %lu KB, its lowest block %lu KB below the top\n",
               (unsigned long)(m.long_bytes / 1024), (unsigned long)((sim_heap.total - m.long_low) / 1024));
#ifdef AU_MEM_TRACK
    fprintf(m.out, "-- platform bytes by the site holding most of each block\n");
    for (int k = 0; k < m.pin.n; k++) {
        int top = -1;
        for (int i = 0; i < m.pin.n; i++)
            if (m.pin.s[i].count && (top < 0 || m.pin.s[i].bytes > m.pin.s[top].bytes))
                top = i;
        if (top < 0)
            break;
        fprintf(m.out, "%8lu KB in %4lu blocks  %s\n", (unsigned long)(m.pin.s[top].bytes / 1024),
                (unsigned long)m.pin.s[top].count, m.pin.s[top].site);
        m.pin.s[top].count = 0;
    }
#endif
    if (m.out != stdout)
        fclose(m.out);
    last = sim_serial;
}

static void au_host_heap_report(const char *label)
{
    if (!au_heap_enabled()) {
        printf("heap (%s): the pool is off; set AURA_HEAP=1\n", label);
        return;
    }
    AuHeapStats st;
    au_heap_stats(&st);
    printf("heap (%s): %lu blocks, %lu KB asked for; %lu KB from the platform in %lu slabs and %lu large blocks\n",
           label, (unsigned long)st.blocks, (unsigned long)(st.used_bytes / 1024),
           (unsigned long)(st.system_bytes / 1024), (unsigned long)st.slabs, (unsigned long)st.large);
    printf("heap (%s): %lu blocks over 2 KB in %lu slabs; %lu KB of empty slabs kept\n", label,
           (unsigned long)st.mid_blocks, (unsigned long)st.mid_slabs, (unsigned long)(st.empty_bytes / 1024));
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: aura-host scene [-o out.png] [-w W] [-h H] [-s scroll] [font options]\n"
            "       aura-host fonts [font options]\n"
            "       aura-host bench [font options]\n"
            "       aura-host font-index DIR [-o FILE]   (writes DIR/fonts.idx)\n"
            "       font options: --font F, --no-system-fonts (or --bundled-only), --no-bundled-fonts,\n"
            "                     --fonts-dir D (default assets/fonts or $AURA_FONTS),\n"
            "                     --color-scheme light|dark (prefers-color-scheme; default light)\n"
            "       aura-host chrome [-o out.png] [-w W] [-h H] [--tabs top|left] [--column W]\n"
            "                        [--tab URL]... [--more-tabs N] [--active N] [--field TEXT] [--focus]\n"
            "                        [--loading] [--hover back|forward|reload|home|bookmark|newtab|splitter]\n"
            "                        [--scroll Y] [--mouse X Y]\n"
            "                        [--native-chrome] (renders only the page, as for a port with its own\n"
            "                        widgets; coordinates are the page's; prints each AU_CHANGED_* notice\n"
            "                        and the chrome's state after the loads and at the end)\n"
            "                        [--profile DIR] (loads and saves Aura's files in DIR as Mac OS 9 does: preferences,\n"
            "                        bookmarks, history, session, web storage in DIR/Local Storage; --net keeps cookies\n"
            "                        and web storage in $AURA_PROFILE)\n"
            "                        (--do storage prints the storage service's areas; --do reload reloads)\n"
            "                        [--net] [--no-doc-cache] [--script-mem MB] [--worker-mem MB] [--no-entropy] [--scroll-bench STEPS [--no-shift]] [--do ACTION]...\n"
            "                        (--do: click:X,Y dclick:X,Y tclick:X,Y clickname:NAME type:TEXT\n"
            "                        key:[cmd-][shift-]NAME\n"
            "                        edit:copy|cut|paste:TEXT|undo|clear|selectall wait:MS pause:MS shot:FILE field\n"
            "                        go:URL newtab:URL back forward closetab mem (bytes live per source line, in a\n"
            "                        build with -DAURA_MEM_TRACK=ON) cache (the HTTP cache's counts with --net,\n"
            "                        the documents kept for Back and Forward, DNS lookups)\n"
            "                        keys:TEXT (types it and prints what each key cost) js:CODE (evaluate\n"
            "                        in the page) save:FILE (the page source) console;\n"
            "                        --no-doc-cache makes Back and Forward load again;\n"
            "                        --script-mem MB and --worker-mem MB set the script memory limits of a\n"
            "                        page and of a worker, else Automatic)\n"
            "                        (developer tools: netlog[:details] perf eval:EXPR source[:FIRST,COUNT]\n"
            "                        devshot:source|console|network|perf:FILE.png)\n"
            "                        (--tab takes a URL, a path, or file:relative/path; --net loads http and\n"
            "                        https over the network, else they show a stand-in; --scroll-bench times\n"
            "                        32-pixel steps and page downs through the Mac shell's paint path,\n"
            "                        repainting the page for each, then moving pixels; --no-shift only\n"
            "                        the first)\n"
            "                        (--blocklist FILE with --net: requests to hosts in the compiled or\n"
            "                        text list FILE are blocked; the counts are printed at the end)\n"
            "       aura-host blocklist compile IN OUT | check FILE HOST... | bench LIST.txt [N] |\n"
            "                        update DIR [--lists \"ID...\"] [--url URL]... [--now]\n"
            "       aura-host render page.html -w W [-h H] [-s scrollY] -o out.png\n"
            "       aura-host dump page.html -w W\n"
            "       aura-host image FILE [-o out.png] [--scale N] [--target WxH] [--chunk N] [--frames]\n"
            "                        [--repeat N] [--max-bytes N] [--favicon SIZE]\n"
            "       aura-host js [--html PAGE.html] [--quiet] [--mem MB] [--kill MS] FILE.js...\n"
            "       aura-host page [--quiet] [--expect-errors N] [--click ID]... PAGE.html\n");
    return 2;
}

static int cmd_scene(AuFontSet *fonts, const char *out, int w, int h, int scroll)
{
    AuSurface *s = au_surface_new(w, h);
    au_host_set_window_surface(s);
    AuCanvas *c = au_canvas_new(s);
    double t0 = now_ms();
    au_scene_draw(c, fonts, w, h, scroll);
    double t1 = now_ms();
    au_platform_present(0, 0, w, h);
    if (!au_png_write(out, s)) {
        fprintf(stderr, "aura-host: cannot write %s\n", out);
        return 1;
    }
    printf("%s: %dx%d, drawn in %.2f ms (cold glyph cache)\n", out, w, h, t1 - t0);
    au_canvas_free(c);
    au_surface_free(s);
    return 0;
}

/* aura-host font-index DIR [-o FILE]: writes the Fonts folder index
 * (default DIR/fonts.idx). */
static int cmd_font_index(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    const char *dir = argv[2];
    char def[2048];
    snprintf(def, sizeof def, "%s/fonts.idx", dir);
    const char *out = def;
    if (argc >= 5 && !strcmp(argv[3], "-o"))
        out = argv[4];
    AuBuf b;
    au_buf_init(&b);
    if (!au_posix_font_index(dir, &b)) {
        fprintf(stderr, "aura-host: no usable fonts in %s\n", dir);
        au_buf_free(&b);
        return 1;
    }
    FILE *f = !strcmp(out, "-") ? stdout : fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "aura-host: cannot write %s\n", out);
        au_buf_free(&b);
        return 1;
    }
    fwrite(b.data, 1, b.len, f);
    if (f != stdout)
        fclose(f);
    au_buf_free(&b);
    return 0;
}

static int cmd_fonts(AuFontSet *fonts)
{
    for (size_t i = 0; i < au_fontset_count(fonts); i++) {
        int weight;
        bool italic;
        const char *fam = au_fontset_entry(fonts, i, &weight, &italic);
        printf("%-28s %3d %s%s\n", fam, weight, italic ? "italic" : "normal",
               (au_fontset_entry_flags(fonts, i) & AU_FONT_BUNDLED) ? "  (bundled)" : "");
    }
    static const char *gen[] = {"serif", "sans-serif", "monospace", "system-ui"};
    for (int g = 0; g < 4; g++) {
        AuFontFace *f = au_fontset_match(fonts, gen[g], 400, false);
        printf("%-12s -> %s\n", gen[g], f ? f->full_name : "(none)");
    }
    return 0;
}

static const char paragraph[] =
    "Aura is a web browser engine for Mac OS 9. It parses HTML and CSS, lays out boxes and paints "
    "them with its own rasterizer and TrueType renderer, so it runs on a 300 MHz G3. ";

static int cmd_bench(AuFontSet *fonts)
{
    AuSurface *s = au_surface_new(640, 720);
    AuCanvas *c = au_canvas_new(s);
    AuFontFace *sans = au_fontset_match(fonts, "sans-serif", 400, false);
    double t0, t;
    int n;

    /* Scene, warm glyph cache. */
    au_scene_draw(c, fonts, 640, 720, 0);
    n = 50;
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_scene_draw(c, fonts, 640, 720, 0);
    t = (now_ms() - t0) / n;
    printf("scene 640x720, warm cache      %8.3f ms\n", t);

    n = 20;
    t0 = now_ms();
    for (int i = 0; i < n; i++) {
        au_glyph_cache_clear();
        au_scene_draw(c, fonts, 640, 720, 0);
    }
    t = (now_ms() - t0) / n;
    printf("scene 640x720, cold cache      %8.3f ms\n", t);

    /* Rectangles. */
    n = 200;
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_rect(c, (AuRect){0, 0, 640, 480}, AU_RGB(10, 20, 30));
    t = (now_ms() - t0) / n;
    printf("fill 640x480 opaque            %8.3f ms  (%.0f Mpx/s)\n", t, 640 * 480 / t / 1000);
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_rect(c, (AuRect){0.5f, 0.5f, 640, 480}, AU_RGBA(10, 20, 30, 128));
    t = (now_ms() - t0) / n;
    printf("fill 640x480 alpha, fractional %8.3f ms  (%.0f Mpx/s)\n", t, 640 * 480 / t / 1000);

    /* Paths. */
    AuPath p;
    au_path_init(&p);
    au_path_ellipse(&p, 320, 300, 200, 200);
    n = 200;
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGBA(200, 30, 30, 200));
    t = (now_ms() - t0) / n;
    printf("fill circle r=200              %8.3f ms  (%.0f Mpx/s)\n", t, 3.14159 * 200 * 200 / t / 1000);
    au_path_clear(&p);
    AuRadii r = {{20, 20, 20, 20}, {20, 20, 20, 20}};
    AuBorder b = {{4, 4, 4, 4}, {AU_RGB(200, 0, 0), AU_RGB(0, 200, 0), AU_RGB(0, 0, 200), AU_RGB(100, 100, 0)}};
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_draw_border(c, (AuRect){20, 20, 300, 200}, &b, &r);
    t = (now_ms() - t0) / n;
    printf("border 300x200 r=20, 4 colours %8.3f ms\n", t);
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_draw_box_shadow(c, (AuRect){40, 40, 300, 200}, &r, 4, 4, 16, 0, AU_RGBA(0, 0, 0, 100), false);
    t = (now_ms() - t0) / n;
    printf("box-shadow 300x200 blur 16     %8.3f ms\n", t);
    AuGradient g = {0};
    au_gradient_css_line(&g, (AuRect){0, 0, 640, 480}, 45);
    au_gradient_add_stop(&g, 0, AU_RGB(255, 0, 0));
    au_gradient_add_stop(&g, 1, AU_RGB(0, 0, 255));
    t0 = now_ms();
    for (int i = 0; i < n; i++)
        au_fill_rect_gradient(c, (AuRect){0, 0, 640, 480}, &g);
    t = (now_ms() - t0) / n;
    printf("gradient 640x480               %8.3f ms\n", t);

    /* Text. */
    if (sans) {
        size_t len = strlen(paragraph);
        size_t glyphs = 0;
        for (size_t i = 0; i < len; i++)
            glyphs += (paragraph[i] & 0xC0) != 0x80;
        n = 200;
        au_draw_text(c, sans, 13, 0, 100, paragraph, len, AU_RGB(0, 0, 0));
        t0 = now_ms();
        for (int i = 0; i < n; i++)
            au_draw_text(c, sans, 13, 0, 100 + (i % 40) * 16, paragraph, len, AU_RGB(0, 0, 0));
        t = (now_ms() - t0) / n;
        printf("text 13px, cached              %8.3f ms per %zu glyphs (%.2f us/glyph)\n", t, glyphs,
               t * 1000 / glyphs);
        n = 50;
        t0 = now_ms();
        for (int i = 0; i < n; i++) {
            au_glyph_cache_clear();
            au_draw_text(c, sans, 13, 0, 100, paragraph, len, AU_RGB(0, 0, 0));
        }
        t = (now_ms() - t0) / n;
        AuGlyphCacheStats st;
        au_glyph_cache_stats(&st);
        printf("text 13px, uncached            %8.3f ms per %zu glyphs (%zu misses total)\n", t, glyphs, st.misses);
        t0 = now_ms();
        for (int i = 0; i < n; i++)
            (void)au_text_width(sans, 13, paragraph, len);
        t = (now_ms() - t0) / n;
        printf("measure 13px                   %8.3f ms per %zu glyphs\n", t, glyphs);
    }
    au_path_free(&p);
    au_canvas_free(c);
    au_surface_free(s);
    return 0;
}

/* ---- chrome: a browser window rendered to PNG ---- */

static uint32_t host_ms(void *ctx)
{
    (void)ctx;
    return au_platform_time_ms();
}

static AuTime host_now(void *ctx)
{
    (void)ctx;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (AuTime)t + (AuTime)lt.tm_gmtoff;
}

static bool host_read_url(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    char *path = au_url_to_path(url, '/');
    if (!path)
        return false;
    bool ok = au_platform_read_file(path, data, len);
    au_free(path);
    return ok;
}

typedef struct ChromeOpts {
    int w, h;
    AuTabPosition pos;
    int column;
    const char *tabs[64];
    int ntabs, extra_tabs, active;
    const char *field;
    bool focus, loading, menus, net;
    bool native_chrome; /* --native-chrome: the page alone; the chrome's state and notices are printed */
    const char *hover;
    int scroll, mouse_x, mouse_y;
    int scroll_bench; /* steps of the scroll benchmark; 0 for none */
    bool no_shift;    /* scroll by repainting the page, not by moving pixels */
    bool no_doc_cache; /* Back and Forward load again (through the HTTP cache's history mode) */
    bool profile;      /* --profile DIR: web storage is kept in the profile folder (as --net keeps it) */
    int script_mem_mb, worker_mem_mb; /* the script memory preferences; 0: Automatic */
    const char *actions[256]; /* --do: scripted input, in order */
    int nactions;
    AuHttpCache *cache; /* --net: an HTTP cache in memory, as the Mac application has */
    const char *blocklist; /* --blocklist FILE: a compiled or text list requests are checked against */
} ChromeOpts;

/* ---- The scroll benchmark: the Mac shell's flush on the host ----
 * The window lives in a surface (the GWorld) and a second surface stands
 * for the screen. Each step scrolls, then flushes as browserwin.c does:
 * the pending shift moves the surface's pixels and the screen's, the
 * damaged rectangles are painted, and they are copied to the screen. */

static AuPresent bench_pres;

static void bench_inval(void *ctx, AuBrowserWindow *w, AuIRect r)
{
    (void)ctx, (void)w;
    au_present_invalidate(&bench_pres, r);
}

static void bench_scroll(void *ctx, AuBrowserWindow *w, AuIRect area, int dx, int dy)
{
    (void)ctx, (void)w;
    au_present_scroll(&bench_pres, area, dx, dy);
}

static void copy_rect(AuSurface *dst, const AuSurface *src, AuIRect r)
{
    for (int y = r.y0; y < r.y1; y++)
        memcpy(dst->pixels + (size_t)y * (size_t)dst->stride + (size_t)r.x0,
               src->pixels + (size_t)y * (size_t)src->stride + (size_t)r.x0, (size_t)(r.x1 - r.x0) * sizeof(AuPixel));
}

typedef struct BenchTimes {
    double model, paint, blit; /* scrolling in the model (hover, layout), painting, copying */
    long painted, copied; /* pixels */
} BenchTimes;

static void bench_flush(AuBrowserWindow *w, AuSurface *s, AuSurface *screen, BenchTimes *bt)
{
    double t0 = now_ms();
    AuPresentRound r;
    au_present_begin(&bench_pres, s, &r);
    AuCanvas *c = au_canvas_new(s);
    for (int i = 0; i < r.n; i++) {
        au_bw_paint(w, c, r.rects[i]);
        bt->painted += (long)(r.rects[i].x1 - r.rects[i].x0) * (r.rects[i].y1 - r.rects[i].y0);
    }
    au_canvas_free(c);
    double t1 = now_ms();
    if (r.shifted)
        au_present_shift_pixels(screen, r.area, r.dx, r.dy);
    for (int i = 0; i < r.n; i++) {
        copy_rect(screen, s, r.rects[i]);
        bt->copied += (long)(r.rects[i].x1 - r.rects[i].x0) * (r.rects[i].y1 - r.rects[i].y0);
    }
    double t2 = now_ms();
    bt->paint += t1 - t0;
    bt->blit += t2 - t1;
}

static void scroll_bench(AuBrowserWindow *w, int W, int H, int steps)
{
    AuSurface *s = au_surface_new(W, H), *screen = au_surface_new(W, H);
    BenchTimes bt;
    memset(&bt, 0, sizeof bt);
    au_present_invalidate(&bench_pres, (AuIRect){0, 0, W, H});
    bench_flush(w, s, screen, &bt);
    printf("first paint: %.2f ms\n", bt.paint);
    AuTab *t = au_bw_active_tab(w);
    int mx, my;
    au_bw_scroll_max(w, &mx, &my);
    int page = (w->layout.page.y1 - w->layout.page.y0) - 40;
    static const char *names[] = {"32-pixel step", "page down"};
    for (int kind = 0; kind < 2; kind++) {
        int d = kind == 0 ? 32 : page;
        au_bw_scroll_to(w, 0, 0);
        bench_flush(w, s, screen, &bt);
        AuGlyphCacheStats g0, g1;
        au_glyph_cache_stats(&g0);
        memset(&bt, 0, sizeof bt);
        int n = 0;
        for (int i = 0; i < steps; i++) {
            int before = t->scroll_y;
            double s0 = now_ms();
            au_bw_scroll_by(w, 0, d);
            bt.model += now_ms() - s0;
            if (t->scroll_y == before)
                break;
            bench_flush(w, s, screen, &bt);
            n++;
        }
        au_glyph_cache_stats(&g1);
        if (!n) {
            printf("%s: the page does not scroll\n", names[kind]);
            continue;
        }
        printf("%s: %.2f ms scroll + %.2f ms paint + %.2f ms copy per step (%d steps; %ld pixels painted, %ld copied per step; "
               "glyph cache %lu hits, %lu misses, %lu evictions, %lu KB)\n",
               names[kind], bt.model / n, bt.paint / n, bt.blit / n, n, bt.painted / n, bt.copied / n,
               (unsigned long)(g1.hits - g0.hits), (unsigned long)(g1.misses - g0.misses),
               (unsigned long)(g1.evictions - g0.evictions), (unsigned long)(g1.bytes / 1024));
    }
    /* Images: page downs to the end and back up, as a reader does. */
    AuPageStats p0, p1;
    au_page_stats(t->page, &p0);
    for (int dir = 1; dir >= -1; dir -= 2)
        for (int i = 0; i < 400; i++) {
            int before = t->scroll_y;
            au_bw_scroll_by(w, 0, dir * page);
            if (t->scroll_y == before)
                break;
            bench_flush(w, s, screen, &bt);
        }
    au_page_stats(t->page, &p1);
    printf("images, down to the end and back: %lu decodes, %lu evictions; %lu KB decoded of a %lu KB budget\n",
           (unsigned long)(p1.image_decodes - p0.image_decodes),
           (unsigned long)(p1.image_evictions - p0.image_evictions), (unsigned long)p1.image_kb,
           (unsigned long)p1.image_budget_kb);
    au_surface_free(s);
    au_surface_free(screen);
}

/* What the window invalidated since the record was cleared (--do keys:). */
static struct {
    int n;
    long area;
    AuIRect u;
    AuIRect r[16];
} key_inv;

/* Set when the window invalidates anything; chrome_present clears it. */
static bool settle_dirty;
static int settle_w, settle_h;

static void key_inval(void *ctx, AuBrowserWindow *w, AuIRect r)
{
    (void)ctx, (void)w;
    if (r.x1 <= r.x0 || r.y1 <= r.y0)
        return;
    settle_dirty = true;
    if (key_inv.n < 16)
        key_inv.r[key_inv.n] = r;
    if (!key_inv.n) {
        key_inv.u = r;
    } else {
        if (r.x0 < key_inv.u.x0) key_inv.u.x0 = r.x0;
        if (r.y0 < key_inv.u.y0) key_inv.u.y0 = r.y0;
        if (r.x1 > key_inv.u.x1) key_inv.u.x1 = r.x1;
        if (r.y1 > key_inv.u.y1) key_inv.u.y1 = r.y1;
    }
    key_inv.n++;
    key_inv.area += (long)(r.x1 - r.x0) * (r.y1 - r.y0);
}

/* Paints the window into a scratch surface when it invalidated anything,
 * as a shell paints after each idle call. Painting is what asks for
 * background images, masks and SVG images at their painted size, so a
 * screenshot taken after a wait shows them as a shell would. */
static void chrome_present(AuBrowserWindow *w)
{
    if (!settle_dirty || settle_w <= 0 || settle_h <= 0)
        return;
    settle_dirty = false;
    AuSurface *s = au_surface_new(settle_w, settle_h);
    if (!s)
        return;
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, settle_w, settle_h});
    au_canvas_free(c);
    au_surface_free(s);
}

/* Runs the idle loop for at least min_ms and then until nothing is busy,
 * at most limit_ms in all. */
static void chrome_settle(AuBrowser *b, AuBrowserWindow *w, AuHostSession *ses, bool net, double min_ms,
                          double limit_ms)
{
    double t0 = now_ms();
    for (;;) {
        double t = now_ms() - t0;
        if (t >= limit_ms || (t >= min_ms && !au_browser_busy(b)))
            break;
        if (net) {
            au_posix_net_wait(ses->host, 10);
        } else {
            struct timespec ts = {0, 10 * 1000000};
            nanosleep(&ts, NULL);
        }
        au_bw_idle(w);
        chrome_present(w);
    }
}

static void chrome_shot(AuBrowserWindow *w, int width, int height, const char *path)
{
    AuSurface *s = au_surface_new(width, height);
    AuCanvas *c = au_canvas_new(s);
    au_bw_paint(w, c, (AuIRect){0, 0, width, height});
    if (!au_png_write(path, s))
        fprintf(stderr, "aura-host: cannot write %s\n", path);
    else
        printf("%s: screenshot\n", path);
    au_canvas_free(c);
    au_surface_free(s);
}

static void chrome_key(AuBrowserWindow *w, int key, uint32_t ch, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.key = key;
    e.ch = ch;
    e.mods = mods;
    au_bw_key(w, &e);
}

/* One --do action: click:X,Y (dclick, tclick for double and triple),
 * type:TEXT, key:[cmd-][shift-][opt-][ctrl-]NAME (a key name or one
 * character), edit:undo|cut|copy|paste:TEXT|clear|selectall, wait:MS
 * (at least MS, then until loads finish), pause:MS (exactly MS, loads
 * running), shot:FILE.png, field (the
 * address field takes the keyboard). */
static void chrome_action(AuBrowser *b, AuBrowserWindow *w, AuHostSession *ses, ChromeOpts *o, const char *a)
{
    char buf[64];
    if (!strncmp(a, "clickname:", 10)) {
        /* click the middle of the element named so (scrolled into view) */
        AuTab *t = au_bw_active_tab(w);
        AuIRect r;
        if (!t || !au_page_control_rect(t->page, a + 10, &r)) {
            printf("clickname: no element named %s\n", a + 10);
            return;
        }
        int ph = w->layout.page.y1 - w->layout.page.y0;
        if (r.y0 < t->scroll_y || r.y1 > t->scroll_y + ph)
            au_bw_scroll_to(w, t->scroll_x, r.y0 - ph / 3);
        int x = (r.x0 + r.x1) / 2 - t->scroll_x + w->layout.page.x0;
        int y = (r.y0 + r.y1) / 2 - t->scroll_y + w->layout.page.y0;
        snprintf(buf, sizeof buf, "click:%d,%d", x, y);
        a = buf;
    }
    if (!strncmp(a, "click:", 6) || !strncmp(a, "dclick:", 7) || !strncmp(a, "tclick:", 7)) {
        int clicks = a[0] == 'd' ? 2 : a[0] == 't' ? 3 : 1;
        int x = 0, y = 0;
        sscanf(strchr(a, ':') + 1, "%d,%d", &x, &y);
        for (int k = 1; k <= clicks; k++) {
            AuEvent e;
            memset(&e, 0, sizeof e);
            e.x = x;
            e.y = y;
            e.clicks = k;
            e.type = AU_EVENT_MOUSE_MOVE;
            au_bw_mouse(w, &e);
            e.type = AU_EVENT_MOUSE_DOWN;
            au_bw_mouse(w, &e);
            e.type = AU_EVENT_MOUSE_UP;
            au_bw_mouse(w, &e);
        }
        au_bw_set_field_focus(w, false);
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "type:", 5)) {
        const char *s = a + 5;
        size_t n = strlen(s), i = 0;
        while (i < n) {
            size_t adv = 1;
            uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
            i += adv ? adv : 1;
            if (cp == '\n')
                chrome_key(w, AU_KEY_RETURN, 0, 0);
            else
                chrome_key(w, 0, cp, 0);
        }
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "key:", 4)) {
        const char *s = a + 4;
        uint32_t mods = 0;
        for (;;) {
            if (!strncmp(s, "cmd-", 4))
                mods |= AU_MOD_COMMAND, s += 4;
            else if (!strncmp(s, "shift-", 6))
                mods |= AU_MOD_SHIFT, s += 6;
            else if (!strncmp(s, "opt-", 4))
                mods |= AU_MOD_OPTION, s += 4;
            else if (!strncmp(s, "ctrl-", 5))
                mods |= AU_MOD_CONTROL, s += 5;
            else
                break;
        }
        static const struct {
            const char *name;
            int key;
        } keys[] = {{"left", AU_KEY_LEFT},       {"right", AU_KEY_RIGHT},       {"up", AU_KEY_UP},
                    {"down", AU_KEY_DOWN},       {"pageup", AU_KEY_PAGE_UP},    {"pagedown", AU_KEY_PAGE_DOWN},
                    {"home", AU_KEY_HOME},       {"end", AU_KEY_END},           {"backspace", AU_KEY_BACKSPACE},
                    {"delete", AU_KEY_DELETE},   {"return", AU_KEY_RETURN},     {"tab", AU_KEY_TAB},
                    {"escape", AU_KEY_ESCAPE},   {"space", -1}};
        int key = 0;
        uint32_t ch = 0;
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
            if (!strcmp(s, keys[k].name))
                key = keys[k].key;
        if (key == -1) {
            key = 0;
            ch = ' ';
        } else if (!key) {
            size_t adv = 0;
            ch = au_utf8_decode(s, strlen(s), &adv);
        }
        chrome_key(w, key, ch, mods);
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "edit:", 5)) {
        const char *c = a + 5;
        int cmd = !strcmp(c, "undo")        ? AU_PAGE_EDIT_UNDO
                  : !strcmp(c, "cut")       ? AU_PAGE_EDIT_CUT
                  : !strcmp(c, "copy")      ? AU_PAGE_EDIT_COPY
                  : !strncmp(c, "paste", 5) ? AU_PAGE_EDIT_PASTE
                  : !strcmp(c, "clear")     ? AU_PAGE_EDIT_CLEAR
                                            : AU_PAGE_EDIT_SELECT_ALL;
        const char *text = !strncmp(c, "paste:", 6) ? c + 6 : "";
        AuBuf out;
        au_buf_init(&out);
        bool ok = au_bw_page_edit(w, cmd, text, strlen(text), &out);
        printf("edit %s: %s%s%s\n", c, ok ? "done" : "not done", out.len ? ", text: " : "", au_buf_cstr(&out));
        au_buf_free(&out);
    } else if (!strncmp(a, "wait:", 5)) {
        chrome_settle(b, w, ses, o->net, atof(a + 5), o->net ? 60000 : 10000);
    } else if (!strncmp(a, "pause:", 6)) {
        double ms = atof(a + 6);
        chrome_settle(b, w, ses, o->net, ms, ms);
    } else if (!strncmp(a, "shot:", 5)) {
        chrome_settle(b, w, ses, o->net, 0, 200);
        chrome_shot(w, o->w, o->h, a + 5);
    } else if (!strncmp(a, "resize:", 7)) {
        /* resize:W,H: the window is resized as the shell does (resize,
         * paint, idle), and the layout passes it cost are printed. */
        int nw = o->w, nh = o->h;
        sscanf(a + 7, "%d,%d", &nw, &nh);
        AuTab *t = au_bw_active_tab(w);
        AuPageStats s0, s1;
        memset(&s0, 0, sizeof s0);
        memset(&s1, 0, sizeof s1);
        if (t)
            au_page_stats(t->page, &s0);
        double t0 = now_ms();
        au_bw_resize(w, nw, nh);
        o->w = nw;
        o->h = nh;
        if (settle_w) {
            settle_w = nw;
            settle_h = nh;
        }
        AuSurface *sf = au_surface_new(nw, nh);
        AuCanvas *c = au_canvas_new(sf);
        au_bw_paint(w, c, (AuIRect){0, 0, nw, nh});
        double t1 = now_ms();
        chrome_settle(b, w, ses, o->net, 20, 400);
        au_bw_paint(w, c, (AuIRect){0, 0, nw, nh});
        au_canvas_free(c);
        au_surface_free(sf);
        if (t)
            au_page_stats(t->page, &s1);
        printf("resize to %dx%d: %lu style and layout passes, %lu ms in them; %.1f ms to the first paint\n", nw, nh,
               (unsigned long)(s1.layouts - s0.layouts), (unsigned long)(s1.layout_total_ms - s0.layout_total_ms),
               t1 - t0);
    } else if (!strcmp(a, "field")) {
        au_bw_set_field_focus(w, true);
    } else if (!strncmp(a, "go:", 3)) {
        au_bw_navigate(w, a + 3);
        chrome_settle(b, w, ses, o->net, 20, o->net ? 60000 : 10000);
    } else if (!strncmp(a, "newtab:", 7)) {
        /* Opens URL in a new tab after the others and selects it. */
        au_bw_new_tab(w, a + 7, true, w->ntabs);
        chrome_settle(b, w, ses, o->net, 20, o->net ? 60000 : 10000);
    } else if (!strcmp(a, "back") || !strcmp(a, "forward")) {
        double t0 = now_ms();
        au_bw_go(w, a[0] == 'b' ? -1 : 1);
        double shown = now_ms() - t0;
        chrome_settle(b, w, ses, o->net, 20, o->net ? 60000 : 10000);
        AuTab *bt = au_bw_active_tab(w);
        AuPageStats ps;
        memset(&ps, 0, sizeof ps);
        if (bt)
            au_page_stats(bt->page, &ps);
        if (ps.restored)
            printf("%s: hiding the page left took %lu ms, building the layout again %lu ms\n", a,
                   (unsigned long)ps.hide_ms, (unsigned long)ps.rebuild_ms);
        printf("%s: %s, %.1f ms in the call, %.0f ms until idle\n", a,
               ps.restored          ? "the kept document was shown"
               : ps.document_cached ? "the page was loaded again, its document from the HTTP cache"
                                    : "the page was loaded again, its document from the network",
               shown, now_ms() - t0);
    } else if (!strcmp(a, "reload")) {
        au_bw_reload(w);
        chrome_settle(b, w, ses, o->net, 20, o->net ? 60000 : 10000);
    } else if (!strcmp(a, "storage")) {
        /* The storage service's counts and its areas (never their contents). */
        AuWebStorageStats st;
        AuStorageAreaInfo in;
        au_web_storage_stats(b->storage, &st);
        printf("storage: %u origins on disk, %u for the session only, %u session areas, %lu bytes (%lu in memory), "
               "%u writes pending\n",
               st.local_areas, st.session_only_areas, st.session_areas, (unsigned long)st.bytes,
               (unsigned long)st.resident, st.pending);
        for (size_t i = 0; au_web_storage_info(b->storage, i, &in); i++)
            printf("  %s %s: %u bytes in %u items%s\n", in.kind == AU_STORAGE_SESSION ? "session" : "local", in.key,
                   in.bytes, in.items, in.session_only ? " (session only)" : "");
    } else if (!strncmp(a, "tab:", 4)) {
        /* Selects tab N (from 0), timed until the tab is idle again: an
         * unloaded tab loads here. */
        double t0 = now_ms();
        int n = atoi(a + 4);
        bool was = n >= 0 && n < w->ntabs && au_page_discarded(w->tabs[n]->page);
        au_bw_select_tab(w, n);
        chrome_settle(b, w, ses, o->net, 20, o->net ? 60000 : 10000);
        printf("tab %d selected%s, %.0f ms until idle\n", n, was ? " (it was unloaded and loaded again)" : "",
               now_ms() - t0);
    } else if (!strncmp(a, "unload:", 7)) {
        /* Unloads background tab N (the tab's Unload Tab command). */
        int n = atoi(a + 7);
        size_t before = n >= 0 && n < w->ntabs ? au_page_memory_estimate(w->tabs[n]->page) +
                                                     au_page_kept_bytes(w->tabs[n]->page)
                                               : 0;
        bool ok = au_bw_unload_tab(w, n);
        printf("unload %d: %s; the tab held about %lu KB, now %lu KB\n", n,
               ok ? "unloaded" : "not unloaded (active, loading, or it holds state)", (unsigned long)(before / 1024),
               (unsigned long)(n >= 0 && n < w->ntabs ? au_page_memory_estimate(w->tabs[n]->page) / 1024 : 0));
    } else if (!strcmp(a, "mem")) {
        au_host_mem_report("now");
        AuAtomStats as;
        au_atom_stats(&as);
        printf("atoms: %u live, %u made at run time (%u permanent), %u scopes; strings %lu KB, tables %lu KB\n",
               as.atoms, as.dynamic, as.permanent, as.scopes, (unsigned long)(as.string_bytes / 1024),
               (unsigned long)(as.table_bytes / 1024));
    } else if (!strcmp(a, "heap")) {
        au_host_heap_report("now");
    } else if (!strncmp(a, "heapmap", 7)) {
        sim_heap_map(a[7] == ':' ? a + 8 : NULL);
    } else if (!strcmp(a, "closetab")) {
        /* Closes the active tab (the window keeps at least one). */
        if (w->ntabs > 1)
            au_bw_close_tab(w, w->active);
        chrome_settle(b, w, ses, o->net, 20, 500);
    } else if (!strcmp(a, "cache")) {
        AuCacheStats cs;
        memset(&cs, 0, sizeof cs);
        if (o->cache)
            au_cache_stats(o->cache, &cs);
        printf("cache: %lu fresh, %lu history, %lu revalidated (%lu not modified), %lu missed, %lu stored, "
               "%lu stale while revalidating\n",
               cs.fresh, cs.history, cs.stale, cs.not_modified, cs.misses, cs.stored, cs.stale_while_revalidate);
        size_t kept_bytes = 0;
        size_t kept = au_browser_kept_documents(b, &kept_bytes);
        printf("documents kept for Back and Forward: %lu, about %lu KB\n", (unsigned long)kept,
               (unsigned long)(kept_bytes / 1024));
        AuTab *ct = au_bw_active_tab(w);
        if (ct) {
            AuPageStats ps;
            au_page_stats(ct->page, &ps);
            printf("page source: %lu bytes, %lu held (compressed in %lu ms); the document is about %lu KB\n",
                   (unsigned long)ps.source_bytes, (unsigned long)ps.source_held, (unsigned long)ps.source_pack_ms,
                   (unsigned long)(au_page_memory_estimate(ct->page) / 1024));
        }
        if (ses->net) {
            AuDnsStats ds;
            au_net_dns_stats(ses->net, &ds);
            printf("dns: %lu lookups, %lu from the cache, %lu shared, %lu requests\n", ds.lookups, ds.hits, ds.shared,
                   ds.requests);
        }
    } else if (!strncmp(a, "keys:", 5)) {
        /* Types TEXT one key at a time and prints what each key cost:
         * the time in the key event and the idle pass after it, the
         * document view's update counts, the rectangles invalidated, and
         * the time to paint them. */
        const char *s = a + 5;
        size_t n = strlen(s), i = 0;
        AuTab *t = au_bw_active_tab(w);
        AuSurface *surf = au_surface_new(o->w, o->h);
        AuCanvas *cv = au_canvas_new(surf);
        au_bw_paint(w, cv, (AuIRect){0, 0, o->w, o->h});
        while (t && i < n) {
            size_t adv = 1;
            uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
            i += adv ? adv : 1;
            AuDocViewStats s0, s1;
            memset(&s0, 0, sizeof s0);
            memset(&s1, 0, sizeof s1);
            au_page_update_stats(t->page, &s0);
            memset(&key_inv, 0, sizeof key_inv);
            double t0 = now_ms();
            chrome_key(w, 0, cp, 0);
            double t1 = now_ms();
            au_bw_idle(w);
            double t2 = now_ms();
            au_page_update_stats(t->page, &s1);
            for (int k = 0; k < key_inv.n && k < 16; k++)
                au_bw_paint(w, cv, key_inv.r[k]);
            double t3 = now_ms();
            printf("key %c: event %.2f ms, idle %.2f ms, paint %.2f ms; %u style passes, %u elements styled, "
                   "%u tree builds, %u subtree builds (%u boxes), %u layouts; %d invalidations, %ld px, union "
                   "%d,%d %dx%d\n",
                   cp < 127 ? (char)cp : '?', t1 - t0, t2 - t1, t3 - t2, s1.style_passes - s0.style_passes,
                   s1.styled_elements - s0.styled_elements, s1.tree_builds - s0.tree_builds,
                   s1.subtree_builds - s0.subtree_builds, s1.subtree_boxes - s0.subtree_boxes,
                   s1.layouts - s0.layouts, key_inv.n, key_inv.area, key_inv.u.x0, key_inv.u.y0,
                   key_inv.u.x1 - key_inv.u.x0, key_inv.u.y1 - key_inv.u.y0);
        }
        au_canvas_free(cv);
        au_surface_free(surf);
    } else if (!strncmp(a, "js:", 3)) {
        AuTab *t = au_bw_active_tab(w);
        AuBuf out;
        au_buf_init(&out);
        bool ok = t && au_page_eval(t->page, a + 3, &out);
        printf("js %s: %s\n", ok ? "=" : "failed", au_buf_cstr(&out));
        au_buf_free(&out);
        chrome_settle(b, w, ses, o->net, 20, 200);
    } else if (!strncmp(a, "save:", 5)) {
        AuTab *t = au_bw_active_tab(w);
        const char *data;
        size_t len;
        FILE *f;
        if (t && au_page_source(t->page, &data, &len) && (f = fopen(a + 5, "wb"))) {
            fwrite(data, 1, len, f);
            fclose(f);
            printf("save: %lu bytes\n", (unsigned long)len);
        } else {
            printf("save: no source\n");
        }
    } else if (!strcmp(a, "console")) {
        AuTab *t = au_bw_active_tab(w);
        size_t n = t ? au_page_console_count(t->page) : 0;
        for (size_t i = 0; i < n; i++) {
            int level;
            const char *line = au_page_console_line(t->page, i, &level);
            printf("console %d: %s\n", level, line ? line : "");
        }
    } else if (!au_host_dev_action(w, o->net ? ses->net : NULL, a)) { /* netlog, perf, eval:, source, devshot: */
        fprintf(stderr, "aura-host: unknown --do action %s\n", a);
    }
    AuTab *t = au_bw_active_tab(w);
    if (t)
        printf("after %s: %s%s\n", a, au_page_url(t->page), au_bw_page_editing(w) ? " (editing a field)" : "");
}

static AuHitPart hit_by_name(const char *s)
{
    static const struct {
        const char *name;
        AuHitPart part;
    } names[] = {{"back", AU_HIT_BACK},       {"forward", AU_HIT_FORWARD}, {"reload", AU_HIT_RELOAD},
                 {"home", AU_HIT_HOME},       {"bookmark", AU_HIT_BOOKMARK}, {"newtab", AU_HIT_NEW_TAB},
                 {"splitter", AU_HIT_SPLITTER}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(names[i].name, s))
            return names[i].part;
    return AU_HIT_NONE;
}

/* ---- --native-chrome: the page alone, and what a port's widgets show ----
 * Each AU_CHANGED_* notice is printed as it arrives; the chrome's state
 * is printed after the loads and at the end. */

static void print_notice(void *ctx, AuBrowserWindow *w, unsigned what)
{
    static const char *const names[] = {"field",   "title",    "layout",   "scroll",   "menus",
                                        "focus-field", "focus-page", "tabs", "tab-info", "active-tab",
                                        "nav",     "status",   "security", "progress", "bookmark"};
    (void)ctx, (void)w;
    printf("notice:");
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (what & (1u << i))
            printf(" %s", names[i]);
    printf("\n");
}

static void print_chrome_state(AuBrowserWindow *w, const char *when)
{
    static const char *const sec[] = {"none", "secure", "not secure", "certificate error", "mixed"};
    printf("chrome state (%s):\n", when);
    for (int i = 0; i < au_bw_tab_count(w); i++) {
        AuChromeTab ti;
        au_bw_tab_info(w, i, &ti);
        printf("  tab %d%s: \"%s\" <%s>%s%s%s", i, i == au_bw_active_index(w) ? " (active)" : "", ti.title, ti.url,
               ti.loading ? " loading" : "", ti.unloaded ? " unloaded" : "", ti.icon ? " icon" : "");
        if (ti.loading)
            printf(" %d%%", (int)(ti.progress * 100.0f + 0.5f));
        printf("\n");
    }
    char title[256], status[300], progress[32];
    au_bw_window_title(w, title, sizeof title);
    au_bw_status_text(w, status, sizeof status);
    au_bw_progress_text(w, progress, sizeof progress);
    AuSecurity s = au_bw_security(w);
    int sx, sy, mx, my;
    au_bw_scroll_pos(w, &sx, &sy);
    au_bw_scroll_max(w, &mx, &my);
    printf("  window title: \"%s\"\n", title);
    printf("  address field: \"%s\"\n", au_bw_field_text(w));
    printf("  back %s, forward %s, %s, bookmarked %s\n", au_bw_can_go(w, -1) ? "on" : "off",
           au_bw_can_go(w, 1) ? "on" : "off", au_bw_loading(w) ? "loading (Stop)" : "not loading (Reload)",
           au_bw_is_bookmarked(w) ? "yes" : "no");
    printf("  security: %s\n", (unsigned)s < sizeof sec / sizeof sec[0] ? sec[s] : "?");
    printf("  status: \"%s\"; progress: \"%s\"; blocked: %d\n", status, progress, au_bw_blocked_count(w));
    printf("  scroll: %d,%d of %d,%d; page %dx%d\n", sx, sy, mx, my, w->layout.page.x1 - w->layout.page.x0,
           w->layout.page.y1 - w->layout.page.y0);
}

/* crypto for pages: the system's random device. --no-entropy makes it
 * answer as the Mac OS 9 pool does before it has gathered enough. */
static bool host_no_entropy;
static bool host_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    if (host_no_entropy)
        return false;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return false;
    size_t got = len ? fread(buf, 1, len, f) : 0;
    fclose(f);
    return got == len;
}

static int cmd_chrome(AuFontSet *fonts, const char *out, ChromeOpts *o)
{
    AuBrowserHost host = {NULL, host_ms, host_now, host_read_url, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    host.random = host_random;
    host.coro = au_posix_coro_ops();
    if (o->scroll_bench) {
        au_present_init(&bench_pres);
        host.invalidate = bench_inval;
        host.scroll_area = bench_scroll;
    } else {
        host.invalidate = key_inval;
        settle_w = o->w;
        settle_h = o->h;
    }
    if (o->native_chrome)
        host.changed = print_notice;
    AuBrowser *b = au_browser_new(fonts, &host);
    b->site_icons = true;
    if (o->no_doc_cache)
        b->prefs.document_cache_pages = 0;
    if (o->script_mem_mb)
        b->prefs.script_memory_kb = au_script_memory_clamp(o->script_mem_mb * 1024);
    if (o->worker_mem_mb)
        b->prefs.worker_memory_kb = au_script_memory_clamp(o->worker_mem_mb * 1024);
    /* --net: http and https through the network stack and page fetcher,
     * as the Mac application does; without it they show the stand-in. */
    AuHostSession ses;
    AuNetLoader *loader = NULL;
    AuNetPageFetcher *pf = NULL;
    struct AuBlocklist *blocklist = NULL;
    memset(&ses, 0, sizeof ses);
    if (o->net) {
        if (!au_host_session_open(&ses, true, false)) {
            fprintf(stderr, "aura-host: cannot set up sockets\n");
            au_browser_free(b);
            return 3;
        }
        au_net_apply_prefs(ses.net, &b->prefs);
        o->cache = au_cache_new(au_net_cache_bytes_from_prefs(&b->prefs));
        au_net_set_cache(ses.net, o->cache);
        au_net_set_trace_headers(ses.net, true); /* --do netlog:details shows headers */
        loader = au_net_loader_new(ses.net);
        if (o->blocklist && (blocklist = au_host_blocklist_open(o->blocklist)) != NULL)
            au_net_loader_set_blocklist(loader, blocklist);
        pf = au_net_page_fetcher_new(loader);
        AuPageFetcher f;
        au_net_page_fetcher_interface(pf, &f);
        au_browser_set_fetcher(b, "http", &f);
        au_browser_set_fetcher(b, "https", &f);
    }
    /* Web storage: in the profile folder with --net (where the cookies
     * are) or --profile DIR, else in memory for the run. */
    struct AuWebStorage *storage = au_host_storage_open(ses.jar, o->net || o->profile);
    au_browser_set_storage(b, storage);
    /* --profile DIR: the browser's files there too (preferences,
     * bookmarks, history, session, downloads, site icons, Running),
     * loaded and saved by the application services as on Mac OS 9. */
    AuApp *app = NULL;
    if (o->profile) {
        AuAppPort port;
        au_host_app_port(&port);
        app = au_app_new(b, &port);
        if (au_app_load(app))
            printf("profile: the last run did not quit\n");
    }
    b->prefs.tab_position = o->pos;
    if (au_host_dark())
        b->prefs.color_scheme = AU_SCHEME_DARK;
    if (o->column)
        b->prefs.tab_column_width = o->column;
    AuBrowserWindow *w = o->native_chrome ? au_browser_window_new_native(b, o->w, o->h)
                                          : au_browser_window_new(b, o->w, o->h);
    w->native_standins = true;
    if (!app)
        au_bookmark_add(&b->bookmarks, NULL, NULL, "Aura", "about:home", 0);
    if (o->ntabs == 0 && o->extra_tabs == 0)
        o->tabs[o->ntabs++] = "about:home";
    for (int i = 0; i < o->ntabs; i++) {
        /* "file:relative/path" and plain paths are made absolute file: URLs. */
        const char *t = o->tabs[i];
        char *url = NULL;
        if (!strncmp(t, "file:", 5) && t[5] != '/')
            t += 5;
        if (!au_url_scheme_len(t, strlen(t))) {
            char path[4096];
            if (t[0] == '/')
                snprintf(path, sizeof path, "%s", t);
            else if (getcwd(path, sizeof path - 2))
                snprintf(path + strlen(path), sizeof path - strlen(path), "/%s", t);
            url = au_url_from_path(path, '/');
        }
        au_bw_new_tab(w, url ? url : o->tabs[i], true, w->ntabs);
        au_free(url);
    }
    static const char *fill[] = {"about:home", "http://example.com/", "about:scene", "about:fonts",
                                 "http://www.macintoshrepository.org/", "about:blank"};
    for (int i = 0; i < o->extra_tabs; i++)
        au_bw_new_tab(w, fill[i % 6], false, w->ntabs);
    if (!o->loading) {
        /* Let loads finish (simulated ones in 5 seconds, network ones in
         * 60). */
        double t0 = now_ms(), limit = o->net ? 60000 : 5000;
        while (au_browser_busy(b) && now_ms() - t0 < limit) {
            if (o->net) {
                au_posix_net_wait(ses.host, 10);
            } else {
                struct timespec ts = {0, 20 * 1000000};
                nanosleep(&ts, NULL);
            }
            au_bw_idle(w);
            chrome_present(w);
        }
        if (o->net)
            printf("loaded in %.0f ms%s\n", now_ms() - t0, au_browser_busy(b) ? " (still busy at the limit)" : "");
    } else {
        double t0 = now_ms();
        while (now_ms() - t0 < 500) {
            struct timespec ts = {0, 20 * 1000000};
            nanosleep(&ts, NULL);
            au_bw_idle(w);
        }
    }
    if (o->active >= 0 && o->active < w->ntabs)
        au_bw_select_tab(w, o->active);
    if (o->native_chrome)
        print_chrome_state(w, "loaded");
    if (o->nactions)
        au_bw_set_field_focus(w, false); /* keys go to the page */
    for (int i = 0; i < o->nactions; i++)
        chrome_action(b, w, &ses, o, o->actions[i]);
    if (o->field)
        au_bw_set_field_text(w, o->field);
    au_bw_set_field_focus(w, o->focus);
    if (o->hover) {
        w->hover.part = hit_by_name(o->hover);
        w->hover.index = -1;
    }
    if (o->scroll)
        au_bw_scroll_to(w, 0, o->scroll);
    if (o->mouse_x >= 0) {
        /* Move the pointer there (hover state, a link's status text). */
        AuEvent e;
        memset(&e, 0, sizeof e);
        e.type = AU_EVENT_MOUSE_MOVE;
        e.x = o->mouse_x;
        e.y = o->mouse_y;
        au_bw_mouse(w, &e);
    }
    if (o->scroll_bench) {
        /* Repainting the page for each step (as before pixels were moved),
         * then moving pixels. */
        b->host.scroll_area = NULL;
        printf("%dx%d, scrolling by repainting the page\n", o->w, o->h);
        scroll_bench(w, o->w, o->h, o->scroll_bench);
        if (!o->no_shift) {
            b->host.scroll_area = bench_scroll;
            printf("%dx%d, scrolling by moving pixels\n", o->w, o->h);
            scroll_bench(w, o->w, o->h, o->scroll_bench);
        }
    }
    AuSurface *s = au_surface_new(o->w, o->h);
    AuCanvas *c = au_canvas_new(s);
    double t0 = now_ms();
    au_bw_paint(w, c, (AuIRect){0, 0, o->w, o->h});
    double t1 = now_ms();
    int rc = 0;
    if (!au_png_write(out, s)) {
        fprintf(stderr, "aura-host: cannot write %s\n", out);
        rc = 1;
    } else {
        printf("%s: %dx%d, %d tabs (%s), painted in %.2f ms\n", out, o->w, o->h, w->ntabs,
               o->pos == AU_TABS_LEFT ? "left" : "top", t1 - t0);
    }
    if (o->native_chrome)
        print_chrome_state(w, "at the end");
    if (getenv("AURA_MEM"))
        au_host_mem_report("after the paint");
    au_canvas_free(c);
    au_surface_free(s);
    if (app) {
        au_app_quit(app); /* while the window is open: the session is saved too */
        au_app_free(app);
    }
    au_browser_free(b);
    au_host_storage_close(storage); /* writes what changed */
    if (o->net) {
        if (blocklist) {
            au_host_blocklist_report(blocklist);
            au_blocklist_free(blocklist);
        }
        au_net_page_fetcher_free(pf);
        au_net_loader_free(loader);
        au_host_session_close(&ses);
        au_cache_free(o->cache);
    }
    /* What is left is what the program keeps for good (fonts, atoms, the
     * user agent sheets, the network stack's tables) and leaks. */
    if (getenv("AURA_MEM_EXIT"))
        au_host_mem_report("after everything was freed");
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    if (getenv("AURA_HEAP") && !sim_heap_start(getenv("AURA_HEAP")))
        au_heap_enable(host_heap_get, host_heap_release);
    const char *cmd = argv[1];
    if (!strcmp(cmd, "fetch"))
        return au_host_fetch_main(argc, argv);
    if (!strcmp(cmd, "blocklist"))
        return au_host_blocklist_main(argc, argv);
    if ((!strcmp(cmd, "render") || !strcmp(cmd, "dump")) && au_host_net_render_wanted(argc, argv))
        return au_host_net_render_main(argc, argv);
    if (!strcmp(cmd, "render") || !strcmp(cmd, "dump"))
        return au_host_render_main(argc, argv);
    if (!strcmp(cmd, "layout-bench"))
        return au_host_layout_bench_main(argc, argv);
    if (!strcmp(cmd, "image"))
        return au_host_image_main(argc, argv);
    if (!strcmp(cmd, "js"))
        return au_host_js_main(argc, argv);
    if (!strcmp(cmd, "page"))
        return au_host_page_main(argc, argv);
    if (!strcmp(cmd, "chrome")) {
        ChromeOpts o;
        memset(&o, 0, sizeof o);
        o.w = 800;
        o.h = 600;
        o.active = -1;
        o.mouse_x = -1;
        const char *out = "chrome.png";
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            int used = au_host_font_option(argc, argv, i);
            if (used)
                i += used - 1;
            else if (!strcmp(a, "-o") && i + 1 < argc)
                out = argv[++i];
            else if (!strcmp(a, "-w") && i + 1 < argc)
                o.w = atoi(argv[++i]);
            else if (!strcmp(a, "-h") && i + 1 < argc)
                o.h = atoi(argv[++i]);
            else if (!strcmp(a, "--tabs") && i + 1 < argc)
                o.pos = !strcmp(argv[++i], "left") ? AU_TABS_LEFT : AU_TABS_TOP;
            else if (!strcmp(a, "--column") && i + 1 < argc)
                o.column = atoi(argv[++i]);
            else if (!strcmp(a, "--tab") && i + 1 < argc && o.ntabs < 64)
                o.tabs[o.ntabs++] = argv[++i];
            else if (!strcmp(a, "--more-tabs") && i + 1 < argc)
                o.extra_tabs = atoi(argv[++i]);
            else if (!strcmp(a, "--active") && i + 1 < argc)
                o.active = atoi(argv[++i]);
            else if (!strcmp(a, "--field") && i + 1 < argc)
                o.field = argv[++i];
            else if (!strcmp(a, "--focus"))
                o.focus = true;
            else if (!strcmp(a, "--loading"))
                o.loading = true;
            else if (!strcmp(a, "--net"))
                o.net = true;
            else if (!strcmp(a, "--native-chrome"))
                o.native_chrome = true;
            else if (!strcmp(a, "--profile") && i + 1 < argc) {
                setenv("AURA_PROFILE", argv[++i], 1);
                o.profile = true;
            }
            else if (!strcmp(a, "--blocklist") && i + 1 < argc)
                o.blocklist = argv[++i];
            else if (!strcmp(a, "--hover") && i + 1 < argc)
                o.hover = argv[++i];
            else if (!strcmp(a, "--scroll") && i + 1 < argc)
                o.scroll = atoi(argv[++i]);
            else if (!strcmp(a, "--scroll-bench") && i + 1 < argc)
                o.scroll_bench = atoi(argv[++i]);
            else if (!strcmp(a, "--no-shift"))
                o.no_shift = true;
            else if (!strcmp(a, "--no-doc-cache"))
                o.no_doc_cache = true;
            else if (!strcmp(a, "--script-mem") && i + 1 < argc)
                o.script_mem_mb = atoi(argv[++i]);
            else if (!strcmp(a, "--worker-mem") && i + 1 < argc)
                o.worker_mem_mb = atoi(argv[++i]);
            else if (!strcmp(a, "--no-entropy"))
                host_no_entropy = true;
            else if (!strcmp(a, "--mouse") && i + 2 < argc) {
                o.mouse_x = atoi(argv[++i]);
                o.mouse_y = atoi(argv[++i]);
            }
            else if (!strcmp(a, "--do") && i + 1 < argc && o.nactions < (int)(sizeof o.actions / sizeof o.actions[0]))
                o.actions[o.nactions++] = argv[++i];
            else
                return usage();
        }
        if (o.w <= 0 || o.h <= 0 || o.w > 8192 || o.h > 8192)
            return usage();
        AuFontSet *fonts = au_fontset_new();
        au_platform_load_fonts(fonts);
        int rc = cmd_chrome(fonts, out, &o);
        au_fontset_free(fonts);
        return rc;
    }
    const char *out = "scene.png";
    int w = 640, h = AU_SCENE_HEIGHT, scroll = 0;
    if (!strcmp(cmd, "font-index"))
        return cmd_font_index(argc, argv);
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int used = au_host_font_option(argc, argv, i);
        if (used)
            i += used - 1;
        else if (!strcmp(a, "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "-w") && i + 1 < argc)
            w = atoi(argv[++i]);
        else if (!strcmp(a, "-h") && i + 1 < argc)
            h = atoi(argv[++i]);
        else if (!strcmp(a, "-s") && i + 1 < argc)
            scroll = atoi(argv[++i]);
        else
            return usage();
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return usage();
    AuFontSet *fonts = au_fontset_new();
    au_platform_load_fonts(fonts);
    int rc;
    if (!strcmp(cmd, "scene"))
        rc = cmd_scene(fonts, out, w, h, scroll);
    else if (!strcmp(cmd, "fonts"))
        rc = cmd_fonts(fonts);
    else if (!strcmp(cmd, "bench"))
        rc = cmd_bench(fonts);
    else
        rc = usage();
    au_fontset_free(fonts);
    return rc;
}
