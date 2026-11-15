/* Deterministic fuzzer for the SVG code: each iteration takes a corpus
 * file (or a generated one), mutates it (bit flips, byte changes, markup
 * bytes, inserts of SVG tokens, deletes, splices, truncation), and runs
 * one of three targets:
 *   - the XML parser alone, with random limits;
 *   - the path parser on a generated or mutated d string, then strokes
 *     the result with random dashes, caps and joins;
 *   - the SVG image pipeline: sniffing, au_svg_doc_new, intrinsic size,
 *     and rasterizing at two random sizes (as the image cache does when an
 *     image is shown larger).
 * Under ASan/UBSan this finds memory errors; on its own it checks
 * termination.
 *
 * usage: fuzz_svg ITERATIONS SEED [SECONDS] CORPUS... */
#include "svgtest.h"

#include <time.h>

static const char *tokens[] = {
    "<svg>", "</svg>", "<g>", "</g>", "<use href='#a'/>", "id='a'", "<defs>", "</defs>", "<rect width='5' height='5'/>",
    "<path d='M0 0L9 9z'/>", "<circle r='4'/>", "<text>x</text>", "<tspan dx='1 2'>y</tspan>", "<clipPath id='c'>",
    "</clipPath>", "clip-path='url(#c)'", "<mask id='m'>", "</mask>", "mask='url(#m)'", "<linearGradient id='g'>",
    "<stop offset='.5' stop-color='red'/>", "</linearGradient>", "fill='url(#g)'", "<symbol id='s' viewBox='0 0 1 1'>",
    "</symbol>", "<switch>", "</switch>", "opacity='.5'", "transform='rotate(45)'", "viewBox='0 0 0 0'",
    "<style>*{fill:red}</style>", "<![CDATA[", "]]>", "<!--", "-->", "&lt;", "&#x10FFFF;", "&#0;", "&amp", "<!DOCTYPE svg [",
    "]>", "<!ENTITY e 'x'>", "&e;", "<image href='data:image/svg+xml,%3Csvg/%3E' width='4' height='4'/>",
    "stroke='black' stroke-dasharray='1 0'", "stroke-width='1e30'", "x='1e38'", "xlink:href='#a'", "<svg x='0' width='-1'>",
    "<radialGradient id='g' fx='2' fy='2' r='0'>", "</radialGradient>", "\"", "'", "<", ">", "=", "/",
};

static const char *path_tokens[] = {"M", "m", "L", "l", "H", "h", "V", "v", "C", "c", "S", "s", "Q", "q", "T", "t", "A",
                                    "a", "Z", "z", " ", ",", "0", "1", "-", ".", "e", "1e38", "-1e-38", "10", "5.5",
                                    "0 0 1 1 1", "1 1 0 1", "nan", "\t"};

static uint8_t *mutate(StRng *r, const uint8_t *src, size_t len, size_t *out_len, uint8_t *const *corpus,
                       const size_t *clens, int nc)
{
    size_t cap = len * 2 + 4096;
    uint8_t *b = malloc(cap);
    memcpy(b, src, len);
    size_t n = len;
    int muts = 1 + (int)(st_rng(r) % 8);
    for (int m = 0; m < muts; m++) {
        uint32_t k = st_rng(r) % 8;
        size_t pos = n ? st_rng(r) % n : 0;
        if (k == 0 && n) {
            b[pos] ^= (uint8_t)(1u << (st_rng(r) % 8));
        } else if (k == 1 && n) {
            b[pos] = (uint8_t)st_rng(r);
        } else if (k == 2 && n) {
            static const char markup[] = "<>/='\"&;#[]!?- \n";
            b[pos] = (uint8_t)markup[st_rng(r) % (sizeof markup - 1)];
        } else if (k == 3 || k == 4) {
            const char *t = tokens[st_rng(r) % (sizeof tokens / sizeof tokens[0])];
            size_t tl = strlen(t);
            if (n + tl < cap) {
                memmove(b + pos + tl, b + pos, n - pos);
                memcpy(b + pos, t, tl);
                n += tl;
            }
        } else if (k == 5 && n) {
            size_t dl = 1 + st_rng(r) % (n - pos < 64 ? n - pos : 64);
            memmove(b + pos, b + pos + dl, n - pos - dl);
            n -= dl;
        } else if (k == 6 && nc > 0) {
            int o = (int)(st_rng(r) % (uint32_t)nc);
            if (clens[o]) {
                size_t from = st_rng(r) % clens[o], cl = 1 + st_rng(r) % (clens[o] - from);
                if (cl > 512)
                    cl = 512;
                if (n + cl < cap) {
                    memmove(b + pos + cl, b + pos, n - pos);
                    memcpy(b + pos, corpus[o] + from, cl);
                    n += cl;
                }
            }
        } else if (k == 7 && n) {
            n = pos;
        }
    }
    *out_len = n;
    return b;
}

static void run_path(StRng *r, const uint8_t *data, size_t len)
{
    char d[1024];
    size_t n = 0;
    if (st_rng(r) % 2 || !len) {
        int count = (int)(st_rng(r) % 60);
        for (int i = 0; i < count; i++) {
            const char *t = path_tokens[st_rng(r) % (sizeof path_tokens / sizeof path_tokens[0])];
            size_t tl = strlen(t);
            if (n + tl >= sizeof d)
                break;
            memcpy(d + n, t, tl);
            n += tl;
        }
    } else {
        n = len < sizeof d - 1 ? len : sizeof d - 1;
        memcpy(d, data, n);
    }
    d[n] = 0;
    AuPath p, o;
    au_path_init(&p);
    au_path_init(&o);
    au_svg_path_parse(d, n, &p);
    AuMatrix m;
    au_svg_transform_parse(d, &m);
    float dashes[4] = {(float)(st_rng(r) % 5), (float)(st_rng(r) % 5) * 0.5f, 1, 0};
    AuStrokeStyle ss = {0};
    ss.width = (float)(st_rng(r) % 8) * 0.75f;
    ss.cap = (uint8_t)(st_rng(r) % 3);
    ss.join = (uint8_t)(st_rng(r) % 3);
    ss.miter_limit = (float)(st_rng(r) % 10);
    if (st_rng(r) % 2) {
        ss.dashes = dashes;
        ss.ndashes = 1 + st_rng(r) % 4;
    }
    ss.tolerance = 0.25f;
    au_path_stroke_style(&p, &ss, &o);
    au_path_free(&p);
    au_path_free(&o);
}

static void run_doc(StRng *r, const uint8_t *data, size_t len)
{
    au_svg_sniff(data, len);
    AuSvgDoc *d = au_svg_doc_new(data, len, (int)(st_rng(r) % 2));
    if (!d)
        return;
    AuSvgIntrinsic in;
    au_svg_doc_intrinsic(d, &in);
    int w, h;
    au_svg_doc_default_size(d, &w, &h);
    int w1 = 1 + (int)(st_rng(r) % 48), h1 = 1 + (int)(st_rng(r) % 48);
    AuSurface *s = au_svg_doc_rasterize(d, w1, h1, NULL);
    if (s)
        au_surface_free(s);
    s = au_svg_doc_rasterize(d, w1 * 2, h1 * 2, NULL);
    if (s)
        au_surface_free(s);
    au_svg_doc_free(d);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: fuzz_svg ITERATIONS SEED [SECONDS] CORPUS...\n");
        return 2;
    }
    long iters = atol(argv[1]);
    StRng r = {(uint32_t)atol(argv[2]) * 2654435761u + 1};
    int first = 3;
    double seconds = 0;
    if (argc > 3 && argv[3][0] >= '0' && argv[3][0] <= '9' && !strchr(argv[3], '/')) {
        seconds = atof(argv[3]);
        first = 4;
    }
    int nc = argc - first;
    uint8_t **corpus = calloc((size_t)(nc > 0 ? nc : 1), sizeof *corpus);
    size_t *clens = calloc((size_t)(nc > 0 ? nc : 1), sizeof *clens);
    int loaded = 0;
    for (int i = 0; i < nc; i++) {
        corpus[loaded] = st_read_file(argv[first + i], &clens[loaded]);
        if (corpus[loaded])
            loaded++;
    }
    static const char seed[] = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'><rect width='5' height='5'/></svg>";
    clock_t t0 = clock();
    long i;
    for (i = 0; i < iters; i++) {
        if (seconds > 0 && (double)(clock() - t0) / CLOCKS_PER_SEC > seconds)
            break;
        const uint8_t *src = (const uint8_t *)seed;
        size_t slen = sizeof seed - 1;
        if (loaded) {
            int k = (int)(st_rng(&r) % (uint32_t)loaded);
            src = corpus[k];
            slen = clens[k];
        }
        size_t n;
        uint8_t *b = mutate(&r, src, slen, &n, corpus, clens, loaded);
        st_write_file("fuzz-svg-last.bin", b, n);
        uint32_t target = st_rng(&r) % 3;
        if (target == 0) {
            AuXmlLimits lim = {st_rng(&r) % 300, st_rng(&r) % 2000, st_rng(&r) % 300};
            AuDocument *d = au_xml_parse((const char *)b, n, st_rng(&r) % 2 ? &lim : NULL, NULL);
            if (d)
                au_node_unref(&d->node);
        } else if (target == 1) {
            run_path(&r, b, n);
        } else {
            run_doc(&r, b, n);
        }
        free(b);
    }
    remove("fuzz-svg-last.bin");
    printf("fuzz_svg: %ld iterations in %.1f s over %d corpus files\n", i, (double)(clock() - t0) / CLOCKS_PER_SEC,
           loaded);
    for (int k = 0; k < loaded; k++)
        free(corpus[k]);
    free(corpus);
    free(clens);
    return 0;
}
