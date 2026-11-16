/* SVG tests: the path and transform grammars, the XML parser (entities,
 * DTDs, limits), and rendering checked by sampling pixels.
 *
 * usage: test_svg [FONT.ttf] [--bench] */
#include "svgtest.h"

#include <math.h>
#include <time.h>

static AuFontSet *g_fonts;

static AuSurface *render(const char *svg, int w, int h)
{
    AuSvgDoc *d = au_svg_doc_new(svg, strlen(svg), 0);
    if (!d)
        return NULL;
    AuSurface *s = au_svg_doc_rasterize(d, w, h, g_fonts);
    au_svg_doc_free(d);
    return s;
}

static AuColor px(const AuSurface *s, int x, int y) { return au_surface_get(s, x, y); }
static int alpha(AuColor c) { return (int)(c >> 24); }
static int red(AuColor c) { return (int)((c >> 16) & 255); }
static int green(AuColor c) { return (int)((c >> 8) & 255); }
static int blue(AuColor c) { return (int)(c & 255); }

static bool near(AuColor c, int a, int r, int g, int b, int tol)
{
    return abs(alpha(c) - a) <= tol && (a == 0 || (abs(red(c) - r) <= tol && abs(green(c) - g) <= tol &&
                                                   abs(blue(c) - b) <= tol));
}

#define EXPECT_PX(s, x, y, a, r, g, b)                                                                      \
    do {                                                                                                    \
        AuColor c_ = px(s, x, y);                                                                           \
        ST_CHECK(near(c_, a, r, g, b, 8), "pixel (%d,%d) is %08x, expected a%d rgb(%d,%d,%d)", x, y,        \
                 (unsigned)c_, a, r, g, b);                                                                 \
    } while (0)

#define SVG(body) "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>" body "</svg>"
#define SVGVB(vb, body)                                                                                     \
    "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' viewBox='" vb      \
    "'>" body "</svg>"

/* ---- Grammars ---- */

static size_t count_ops(const AuPath *p, int op)
{
    size_t n = 0;
    for (size_t i = 0; i < p->nops; i++)
        n += p->ops[i] == op;
    return n;
}

static void test_path_grammar(void)
{
    AuPath p;
    au_path_init(&p);
    ST_CHECK(au_svg_path_parse("M10 10h10v10z", 13, &p), "simple path");
    ST_CHECK(count_ops(&p, AU_PATH_MOVE) == 1 && count_ops(&p, AU_PATH_LINE) == 2 && count_ops(&p, AU_PATH_CLOSE) == 1,
             "simple path ops");
    au_path_free(&p);

    /* Implicit lineto after moveto, packed numbers, exponents. */
    au_path_init(&p);
    const char *d = "m1.5.5.5.5-1e1,2E-1";
    ST_CHECK(au_svg_path_parse(d, strlen(d), &p), "packed numbers");
    ST_CHECK(p.nops == 3 && p.ops[1] == AU_PATH_LINE, "implicit lineto, got %zu ops", p.nops);
    if (p.npts >= 6)
        ST_CHECK(fabsf(p.pts[2] - 2.0f) < 1e-4f && fabsf(p.pts[3] - 1.0f) < 1e-4f && fabsf(p.pts[4] + 8.0f) < 1e-4f &&
                     fabsf(p.pts[5] - 1.2f) < 1e-4f,
                 "relative implicit lineto");
    au_path_free(&p);

    /* Arc flags without separators. */
    au_path_init(&p);
    d = "M0 0a10 10 0 1110 10";
    ST_CHECK(au_svg_path_parse(d, strlen(d), &p), "arc flags packed");
    ST_CHECK(count_ops(&p, AU_PATH_CUBIC) >= 2, "arc becomes cubics");
    au_path_free(&p);

    /* Smooth curves reflect, quadratics. */
    au_path_init(&p);
    d = "M0 0C10 0 10 10 20 10S30 20 40 20Q50 0 60 0T80 0";
    ST_CHECK(au_svg_path_parse(d, strlen(d), &p), "smooth curves");
    ST_CHECK(count_ops(&p, AU_PATH_CUBIC) == 2 && count_ops(&p, AU_PATH_QUAD) == 2, "smooth ops");
    if (p.npts >= 8)
        ST_CHECK(p.pts[8] == 30 && p.pts[9] == 10, "S reflects the control point (%g,%g)", p.pts[8], p.pts[9]);
    au_path_free(&p);

    /* Errors keep what came before. */
    au_path_init(&p);
    d = "M0 0L10 10L20 20x30 30";
    ST_CHECK(!au_svg_path_parse(d, strlen(d), &p), "error detected");
    ST_CHECK(p.nops == 3, "path up to the error: %zu ops", p.nops);
    au_path_free(&p);
    au_path_init(&p);
    d = "M0 0L10 10L20";
    ST_CHECK(!au_svg_path_parse(d, strlen(d), &p) && p.nops == 2, "an incomplete segment is dropped");
    au_path_free(&p);

    au_path_init(&p);
    ST_CHECK(!au_svg_path_parse("L10 10", 6, &p) && p.nops == 0, "must start with moveto");
    au_path_free(&p);

    /* A zero radius arc is a line; huge numbers clamp. */
    au_path_init(&p);
    d = "M0 0A0 5 0 0 0 10 10M1e40 1e40L-1e40 0";
    au_svg_path_parse(d, strlen(d), &p);
    ST_CHECK(p.nops >= 2 && p.ops[1] == AU_PATH_LINE, "zero radius arc is a line");
    for (size_t i = 0; i < p.npts; i++)
        ST_CHECK(isfinite(p.pts[i]) && fabsf(p.pts[i]) <= 1e9f, "points finite and clamped");
    au_path_free(&p);
}

static void test_transform_grammar(void)
{
    AuMatrix m;
    ST_CHECK(au_svg_transform_parse("translate(10,20) scale(2)", &m), "transform list");
    ST_CHECK(m.a == 2 && m.d == 2 && m.e == 10 && m.f == 20, "translate scale: %g %g %g %g", m.a, m.d, m.e, m.f);
    ST_CHECK(au_svg_transform_parse("rotate(90 10 10)", &m), "rotate about a point");
    float X = m.a * 20 + m.c * 10 + m.e, Y = m.b * 20 + m.d * 10 + m.f;
    ST_CHECK(fabsf(X - 10) < 1e-3f && fabsf(Y - 20) < 1e-3f, "rotate(90 10 10) maps (20,10) to (%g,%g)", X, Y);
    ST_CHECK(au_svg_transform_parse("matrix(1 0 0 1 5 6),skewX(0)", &m) && m.e == 5 && m.f == 6, "matrix, comma");
    ST_CHECK(!au_svg_transform_parse("translate(10", &m) && m.a == 1 && m.e == 0, "unterminated is ignored");
    ST_CHECK(!au_svg_transform_parse("foo(1)", &m), "unknown function");
}

/* ---- XML ---- */

static AuElement *first_child_el(AuElement *e)
{
    for (AuNode *c = e->node.first_child; c; c = c->next)
        if (c->type == AU_NODE_ELEMENT)
            return au_as_element(c);
    return NULL;
}

static const char *text_of(AuElement *e)
{
    for (AuNode *c = e->node.first_child; c; c = c->next)
        if (c->type == AU_NODE_TEXT)
            return au_as_text(c)->data;
    return "";
}

static void test_xml(void)
{
    const char *err = NULL;
    const char *a = "<?xml version='1.0'?><!-- c --><svg xmlns='http://www.w3.org/2000/svg'><text>a&lt;b&#x41;&#66;&amp;&unknown;</text></svg>";
    AuDocument *d = au_xml_parse(a, strlen(a), NULL, &err);
    ST_CHECK(d, "basic document: %s", err ? err : "");
    if (d) {
        AuElement *root = au_document_element(d);
        ST_CHECK(root && root->ns == AU_NS_SVG, "root in the SVG namespace");
        AuElement *t = root ? first_child_el(root) : NULL;
        ST_CHECK(t && !strcmp(text_of(t), "a<bAB&&unknown;"), "entities: '%s'", t ? text_of(t) : "");
        au_node_unref(&d->node);
    }

    /* External entities and the internal subset are never read. */
    const char *x = "<?xml version='1.0'?><!DOCTYPE svg [<!ENTITY xxe SYSTEM 'file:///etc/passwd'>"
                    "<!ENTITY a 'AAAA'><!ENTITY b '&a;&a;&a;&a;'>]>"
                    "<svg xmlns='http://www.w3.org/2000/svg'><text>&xxe;&b;</text></svg>";
    d = au_xml_parse(x, strlen(x), NULL, &err);
    ST_CHECK(d, "doctype with internal subset parses");
    if (d) {
        AuElement *t = first_child_el(au_document_element(d));
        ST_CHECK(t && !strcmp(text_of(t), "&xxe;&b;"), "declared entities do not expand: '%s'", t ? text_of(t) : "");
        au_node_unref(&d->node);
    }

    const char *bad[] = {
        "<svg><g></svg>",                     /* mismatched */
        "<svg a='1' a='2'/>",                 /* duplicate attribute */
        "<svg>",                              /* unclosed */
        "",                                   /* empty */
        "<!-- only a comment -->",            /* no root */
        "<svg/><svg/>",                       /* two roots */
        "<svg a=1/>",                         /* unquoted */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        d = au_xml_parse(bad[i], strlen(bad[i]), NULL, NULL);
        ST_CHECK(!d, "malformed %zu rejected", i);
        if (d)
            au_node_unref(&d->node);
    }

    /* Depth limit. */
    char *deep = malloc(300 * 3 + 300 * 4 + 64);
    size_t dn = 0;
    memcpy(deep, "<svg>", 5);
    dn = 5;
    for (int i = 0; i < 300; i++, dn += 3)
        memcpy(deep + dn, "<g>", 3);
    for (int i = 0; i < 300; i++, dn += 4)
        memcpy(deep + dn, "</g>", 4);
    memcpy(deep + dn, "</svg>", 7);
    d = au_xml_parse(deep, strlen(deep), NULL, &err);
    ST_CHECK(!d, "nesting over the limit rejected");
    AuXmlLimits lim = {400, 0, 0};
    d = au_xml_parse(deep, strlen(deep), &lim, &err);
    ST_CHECK(d, "nesting under a raised limit accepted");
    if (d)
        au_node_unref(&d->node);
    free(deep);

    /* CDATA and xlink:href. */
    const char *c = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>"
                    "<style><![CDATA[rect{fill:red}]]></style><use xlink:href='#a'/></svg>";
    d = au_xml_parse(c, strlen(c), NULL, &err);
    ST_CHECK(d, "CDATA document");
    if (d) {
        AuElement *s = first_child_el(au_document_element(d));
        ST_CHECK(s && !strcmp(text_of(s), "rect{fill:red}"), "CDATA text '%s'", s ? text_of(s) : "");
        AuElement *u = s ? au_as_element(s->node.next) : NULL;
        ST_CHECK(u && au_element_get_attr(u, au_atom_cstr("xlink:href")), "xlink:href kept");
        au_node_unref(&d->node);
    }
}

static void test_sniff(void)
{
    ST_CHECK(au_svg_sniff("<svg>", 5), "plain svg");
    ST_CHECK(au_svg_sniff("\xEF\xBB\xBF  <?xml version='1.0'?>\n<!-- x --><svg ", 42), "bom, xml decl, comment");
    ST_CHECK(au_svg_sniff("<!DOCTYPE svg PUBLIC>", 21), "doctype svg");
    ST_CHECK(!au_svg_sniff("<html>", 6), "html");
    ST_CHECK(!au_svg_sniff("\x89PNG\r\n", 6), "png");
    ST_CHECK(!au_svg_sniff("<svgfoo>", 8), "other element");

    uint8_t *out;
    size_t n;
    char mime[32];
    ST_CHECK(au_svg_data_url("data:image/svg+xml;base64,PHN2Zy8+", 100, &out, &n, mime, sizeof mime) && n == 6 &&
                 !memcmp(out, "<svg/>", 6) && !strcmp(mime, "image/svg+xml"),
             "base64 data URL");
    au_free(out);
    ST_CHECK(au_svg_data_url("data:image/svg+xml,%3Csvg%2F%3E", 100, &out, &n, mime, sizeof mime) && n == 6 &&
                 !memcmp(out, "<svg/>", 6),
             "percent-encoded data URL");
    au_free(out);
    ST_CHECK(!au_svg_data_url("data:image/svg+xml,%3Csvg%2F%3E", 3, &out, &n, mime, sizeof mime), "over the cap");
    ST_CHECK(!au_svg_data_url("http://x/a.svg", 100, &out, &n, mime, sizeof mime), "not a data URL");
}

/* ---- Rendering ---- */

static void test_shapes(void)
{
    AuSurface *s = render(SVG("<rect width='10' height='10' fill='red'/>"
                              "<circle cx='30' cy='10' r='8' fill='#00f'/>"
                              "<ellipse cx='10' cy='30' rx='8' ry='4' fill='lime'/>"
                              "<line x1='20' y1='30' x2='40' y2='30' stroke='black' stroke-width='4'/>"
                              "<polygon points='0,40 10,40 10,50 0,50' fill='red'/>"
                              "<polyline points='20,40 30,40 30,50' fill='none' stroke='#00f' stroke-width='2'/>"),
                          40, 50);
    ST_CHECK(s, "shapes rendered");
    if (!s)
        return;
    EXPECT_PX(s, 5, 5, 255, 255, 0, 0);
    EXPECT_PX(s, 15, 5, 0, 0, 0, 0);
    EXPECT_PX(s, 30, 10, 255, 0, 0, 255);
    EXPECT_PX(s, 23, 3, 0, 0, 0, 0);
    EXPECT_PX(s, 10, 30, 255, 0, 255, 0);
    EXPECT_PX(s, 10, 35, 0, 0, 0, 0);
    EXPECT_PX(s, 30, 30, 255, 0, 0, 0);
    EXPECT_PX(s, 30, 33, 0, 0, 0, 0);
    EXPECT_PX(s, 5, 45, 255, 255, 0, 0);
    EXPECT_PX(s, 25, 40, 255, 0, 0, 255);
    EXPECT_PX(s, 25, 45, 0, 0, 0, 0);
    au_surface_free(s);

    /* Default fill is black; rounded rect corners; zero sizes draw nothing. */
    s = render(SVG("<rect width='20' height='20' rx='10'/><rect x='20' width='0' height='20' fill='red'/>"), 20, 20);
    EXPECT_PX(s, 10, 10, 255, 0, 0, 0);
    EXPECT_PX(s, 1, 1, 0, 0, 0, 0);
    au_surface_free(s);
}

static void test_viewbox(void)
{
    AuSurface *s = render(SVGVB("0 0 10 10", "<rect width='5' height='5' fill='#00f'/>"), 100, 100);
    EXPECT_PX(s, 40, 40, 255, 0, 0, 255);
    EXPECT_PX(s, 60, 60, 0, 0, 0, 0);
    au_surface_free(s);
    /* meet: centred horizontally. */
    s = render(SVGVB("0 0 10 10", "<rect width='10' height='10' fill='red'/>"), 200, 100);
    EXPECT_PX(s, 40, 50, 0, 0, 0, 0);
    EXPECT_PX(s, 60, 50, 255, 255, 0, 0);
    au_surface_free(s);
    s = render("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10' preserveAspectRatio='none'>"
               "<rect width='10' height='10' fill='red'/></svg>",
               200, 100);
    EXPECT_PX(s, 5, 50, 255, 255, 0, 0);
    au_surface_free(s);
    s = render("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10' preserveAspectRatio='xMinYMin slice'>"
               "<rect width='10' height='5' fill='red'/></svg>",
               200, 100);
    EXPECT_PX(s, 100, 90, 255, 255, 0, 0);
    au_surface_free(s);
    /* A zero viewBox disables rendering. */
    s = render(SVGVB("0 0 0 10", "<rect width='10' height='10' fill='red'/>"), 20, 20);
    EXPECT_PX(s, 5, 5, 0, 0, 0, 0);
    au_surface_free(s);
}

static void test_strokes(void)
{
    AuSurface *s = render(SVG("<path d='M0 10H40' stroke='black' stroke-width='4' stroke-dasharray='10 10'/>"
                              "<rect x='5' y='20' width='30' height='15' fill='none' stroke='red' stroke-width='2'/>"
                              "<path d='M0 0H40V40H0z M10 10V30H30V10z' fill='#00f' fill-rule='evenodd' "
                              "transform='translate(40 0)'/>"),
                          80, 40);
    EXPECT_PX(s, 5, 10, 255, 0, 0, 0);
    EXPECT_PX(s, 15, 10, 0, 0, 0, 0);
    EXPECT_PX(s, 25, 10, 255, 0, 0, 0);
    EXPECT_PX(s, 5, 27, 255, 255, 0, 0);
    EXPECT_PX(s, 20, 27, 0, 0, 0, 0);
    EXPECT_PX(s, 45, 5, 255, 0, 0, 255);
    EXPECT_PX(s, 60, 20, 0, 0, 0, 0);
    au_surface_free(s);

    /* Non-scaling stroke: 1 px wide under scale(10). */
    s = render(SVG("<g transform='scale(10)'><path d='M0 2.05H4' stroke='black' stroke-width='1' "
                   "vector-effect='non-scaling-stroke'/></g>"),
               40, 40);
    EXPECT_PX(s, 20, 20, 255, 0, 0, 0);
    EXPECT_PX(s, 20, 23, 0, 0, 0, 0);
    au_surface_free(s);

    /* paint-order: the fill on top of the stroke. */
    s = render(SVG("<rect x='10' y='10' width='20' height='20' fill='red' stroke='#00f' stroke-width='8' "
                   "paint-order='stroke'/>"),
               40, 40);
    EXPECT_PX(s, 12, 20, 255, 255, 0, 0);
    EXPECT_PX(s, 8, 20, 255, 0, 0, 255);
    au_surface_free(s);

    /* Square caps and round joins reach past the ends. */
    s = render(SVG("<path d='M10 10H30' stroke='black' stroke-width='6' stroke-linecap='square'/>"), 40, 20);
    EXPECT_PX(s, 8, 10, 255, 0, 0, 0);
    au_surface_free(s);
}

static void test_gradients(void)
{
    AuSurface *s = render(SVG("<defs><linearGradient id='g'><stop offset='0' stop-color='red'/>"
                              "<stop offset='1' stop-color='#00f'/></linearGradient>"
                              "<linearGradient id='h' href='#g' x2='0' y2='1'/>"
                              "<radialGradient id='r' cx='0.5' cy='0.5' r='0.5'><stop offset='0' stop-color='white'/>"
                              "<stop offset='1' stop-color='black'/></radialGradient></defs>"
                              "<rect width='100' height='20' fill='url(#g)'/>"
                              "<rect y='20' width='20' height='100' fill='url(#h)'/>"
                              "<rect x='40' y='40' width='40' height='40' fill='url(#r)'/>"
                              "<rect x='20' y='20' width='20' height='20' fill='url(#missing) lime'/>"),
                          100, 120);
    AuColor l = px(s, 1, 10), r = px(s, 98, 10), m = px(s, 50, 10);
    ST_CHECK(red(l) > 240 && blue(l) < 15, "gradient starts red: %08x", (unsigned)l);
    ST_CHECK(blue(r) > 240 && red(r) < 15, "gradient ends blue: %08x", (unsigned)r);
    ST_CHECK(abs(red(m) - blue(m)) < 20, "gradient middle: %08x", (unsigned)m);
    AuColor t = px(s, 10, 21), b = px(s, 10, 118);
    ST_CHECK(red(t) > 230 && blue(b) > 230, "href inherits stops, vertical: %08x %08x", (unsigned)t, (unsigned)b);
    AuColor c = px(s, 60, 60), e = px(s, 41, 41);
    ST_CHECK(red(c) > 230 && red(e) < 20, "radial: centre %08x corner %08x", (unsigned)c, (unsigned)e);
    EXPECT_PX(s, 30, 30, 255, 0, 255, 0);
    au_surface_free(s);

    /* userSpaceOnUse with spreadMethod reflect and a focal point. */
    s = render(SVG("<defs><linearGradient id='g' gradientUnits='userSpaceOnUse' x1='0' x2='10' spreadMethod='reflect'>"
                   "<stop offset='0' stop-color='red'/><stop offset='1' stop-color='#00f'/></linearGradient>"
                   "<radialGradient id='f' fx='0.2' fy='0.5'><stop offset='0' stop-color='white'/>"
                   "<stop offset='1' stop-color='black'/></radialGradient></defs>"
                   "<rect width='40' height='10' fill='url(#g)'/>"
                   "<rect y='10' width='40' height='40' fill='url(#f)'/>"),
               40, 50);
    AuColor a = px(s, 1, 5), z = px(s, 19, 5);
    ST_CHECK(red(a) > 200 && red(z) > 200, "reflect returns to red at 20: %08x %08x", (unsigned)a, (unsigned)z);
    AuColor fl = px(s, 9, 30), fr = px(s, 31, 30);
    ST_CHECK(red(fl) > red(fr) + 40, "focal point to the left: %08x %08x", (unsigned)fl, (unsigned)fr);
    au_surface_free(s);
}

static void test_use(void)
{
    AuSurface *s = render(SVG("<defs><rect id='r' width='10' height='10'/>"
                              "<symbol id='s' viewBox='0 0 1 1'><rect width='1' height='1' fill='lime'/></symbol></defs>"
                              "<use href='#r' fill='red'/>"
                              "<use xlink:href='#r' x='20' fill='#00f'/>"
                              "<use href='#s' x='0' y='20' width='20' height='20'/>"),
                          40, 40);
    EXPECT_PX(s, 5, 5, 255, 255, 0, 0);
    EXPECT_PX(s, 25, 5, 255, 0, 0, 255);
    EXPECT_PX(s, 15, 35, 255, 0, 255, 0);
    au_surface_free(s);

    /* Cycles terminate. */
    s = render(SVG("<use id='u' href='#u'/><g id='a'><use href='#b'/></g><g id='b'><use href='#a'/></g>"
                   "<g id='c'><use href='#c'/><rect width='5' height='5' fill='red'/></g>"),
               10, 10);
    EXPECT_PX(s, 2, 2, 255, 255, 0, 0);
    au_surface_free(s);

    /* Exponential use fan-out stays bounded. */
    char buf[4096];
    size_t bn = (size_t)snprintf(buf, sizeof buf, "<svg xmlns='http://www.w3.org/2000/svg'><defs><rect id='l0' width='1' height='1'/>");
    for (int i = 1; i < 20; i++)
        bn += (size_t)snprintf(buf + bn, sizeof buf - bn,
                               "<g id='l%d'><use href='#l%d'/><use href='#l%d'/><use href='#l%d'/></g>", i, i - 1, i - 1,
                               i - 1);
    snprintf(buf + bn, sizeof buf - bn, "</defs><use href='#l19'/></svg>");
    clock_t t0 = clock();
    s = render(buf, 10, 10);
    double ms = (double)(clock() - t0) * 1000 / CLOCKS_PER_SEC;
    ST_CHECK(s && ms < 5000, "use fan-out bounded (%.0f ms)", ms);
    au_surface_free(s);
}

static void test_clip_mask(void)
{
    AuSurface *s = render(SVG("<defs><clipPath id='c'><circle cx='20' cy='20' r='10'/></clipPath>"
                              "<clipPath id='r'><rect x='50' y='10' width='10' height='20'/></clipPath>"
                              "<clipPath id='o' clipPathUnits='objectBoundingBox'><rect width='0.5' height='1'/></clipPath>"
                              "<mask id='m'><rect x='0' y='40' width='20' height='40' fill='white'/></mask>"
                              "<mask id='a' style='mask-type:alpha'><rect x='40' y='40' width='20' height='40' fill='#000'/></mask></defs>"
                              "<rect width='40' height='40' fill='red' clip-path='url(#c)'/>"
                              "<rect x='40' width='40' height='40' fill='#00f' clip-path='url(#r)'/>"
                              "<rect x='80' width='20' height='40' fill='lime' clip-path='url(#o)'/>"
                              "<rect y='40' width='40' height='40' fill='red' mask='url(#m)'/>"
                              "<rect x='40' y='40' width='40' height='40' fill='#00f' mask='url(#a)'/>"),
                          100, 80);
    EXPECT_PX(s, 20, 20, 255, 255, 0, 0);
    EXPECT_PX(s, 3, 3, 0, 0, 0, 0);
    EXPECT_PX(s, 55, 20, 255, 0, 0, 255);
    EXPECT_PX(s, 45, 20, 0, 0, 0, 0);
    EXPECT_PX(s, 85, 20, 255, 0, 255, 0);
    EXPECT_PX(s, 95, 20, 0, 0, 0, 0);
    EXPECT_PX(s, 10, 60, 255, 255, 0, 0);
    EXPECT_PX(s, 30, 60, 0, 0, 0, 0);
    EXPECT_PX(s, 50, 60, 255, 0, 0, 255);
    EXPECT_PX(s, 70, 60, 0, 0, 0, 0);
    au_surface_free(s);

    /* A luminance mask of 50% grey halves the alpha (luminance of the
     * sRGB values, as Chrome computes it). */
    s = render(SVG("<mask id='m'><rect width='20' height='20' fill='#808080'/></mask>"
                   "<rect width='20' height='20' fill='black' mask='url(#m)'/>"),
               20, 20);
    AuColor c = px(s, 10, 10);
    ST_CHECK(abs(alpha(c) - 128) < 6, "grey luminance mask: alpha %d", alpha(c));
    au_surface_free(s);
}

static void test_opacity_style(void)
{
    AuSurface *s = render(SVG("<style>.c{fill:#0f0} #i{fill:#00f}</style>"
                              "<g opacity='0.5'><rect width='20' height='20' fill='red'/>"
                              "<rect x='10' width='20' height='20' fill='red'/></g>"
                              "<rect y='20' width='10' height='10' class='c'/>"
                              "<rect x='10' y='20' width='10' height='10' id='i'/>"
                              "<rect x='20' y='20' width='10' height='10' style='fill:red;fill-opacity:.5'/>"
                              "<rect x='30' y='20' width='10' height='10' display='none'/>"
                              "<rect x='30' y='30' width='10' height='10' visibility='hidden'/>"),
                          40, 40);
    AuColor o = px(s, 15, 10);
    ST_CHECK(abs(alpha(o) - 128) < 8, "group opacity does not double in the overlap: %d", alpha(o));
    EXPECT_PX(s, 5, 25, 255, 0, 255, 0);
    EXPECT_PX(s, 15, 25, 255, 0, 0, 255);
    ST_CHECK(abs(alpha(px(s, 25, 25)) - 128) < 8, "fill-opacity");
    EXPECT_PX(s, 35, 25, 0, 0, 0, 0);
    EXPECT_PX(s, 35, 35, 0, 0, 0, 0);
    au_surface_free(s);

    /* Inheritance through g, currentColor, and CSS transform. */
    s = render(SVG("<g fill='currentColor' style='color:#00f'><rect width='10' height='10'/></g>"
                   "<rect width='10' height='10' fill='red' style='transform:translate(20px,0)'/>"
                   "<rect width='10' height='10' fill='lime' transform='rotate(90) translate(0 -40)'/>"),
               40, 20);
    EXPECT_PX(s, 5, 5, 255, 0, 0, 255);
    EXPECT_PX(s, 25, 5, 255, 255, 0, 0);
    EXPECT_PX(s, 35, 5, 255, 0, 255, 0);
    au_surface_free(s);
}

static void test_nested_switch_image(void)
{
    AuSurface *s = render(SVG("<svg x='10' y='10' width='10' height='10' viewBox='0 0 1 1'>"
                              "<rect width='2' height='2' fill='red'/></svg>"
                              "<switch><rect systemLanguage='xx' width='10' height='10' fill='red'/>"
                              "<rect y='20' width='10' height='10' fill='lime'/><rect y='20' width='10' height='10' fill='red'/></switch>"
                              "<image x='20' y='20' width='10' height='10' "
                              "href='data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22%3E%3Crect width=%22100%25%22 height=%22100%25%22 fill=%22%2300f%22/%3E%3C/svg%3E'/>"
                              "<image x='0' y='0' width='10' height='10' href='http://example.com/x.png'/>"),
                          40, 40);
    EXPECT_PX(s, 15, 15, 255, 255, 0, 0);
    EXPECT_PX(s, 25, 15, 0, 0, 0, 0); /* the nested svg clips its overflow */
    EXPECT_PX(s, 5, 25, 255, 0, 255, 0);
    EXPECT_PX(s, 5, 5, 0, 0, 0, 0);
    EXPECT_PX(s, 25, 25, 255, 0, 0, 255);
    au_surface_free(s);
}

static void test_intrinsic(void)
{
    const char *a = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'/>";
    AuSvgDoc *d = au_svg_doc_new(a, strlen(a), 0);
    AuSvgIntrinsic in;
    int w, h;
    au_svg_doc_intrinsic(d, &in);
    ST_CHECK(in.width == 40 && in.height == 20 && fabsf(in.ratio - 2) < 1e-4f, "width and height");
    au_svg_doc_free(d);
    a = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 10'/>";
    d = au_svg_doc_new(a, strlen(a), 0);
    au_svg_doc_intrinsic(d, &in);
    au_svg_doc_default_size(d, &w, &h);
    ST_CHECK(in.width == 0 && fabsf(in.ratio - 3) < 1e-4f && w == 300 && h == 100, "viewBox ratio: %dx%d", w, h);
    au_svg_doc_free(d);
    a = "<svg xmlns='http://www.w3.org/2000/svg' width='100%'/>";
    d = au_svg_doc_new(a, strlen(a), 0);
    au_svg_doc_default_size(d, &w, &h);
    ST_CHECK(w == 300 && h == 150, "percentages are not intrinsic: %dx%d", w, h);
    au_svg_doc_free(d);
    a = "<svg xmlns='http://www.w3.org/2000/svg' width='2em' height='10mm'/>";
    d = au_svg_doc_new(a, strlen(a), 0);
    au_svg_doc_intrinsic(d, &in);
    ST_CHECK(fabsf(in.width - 32) < 0.01f && fabsf(in.height - 37.795f) < 0.01f, "units: %g %g", in.width, in.height);
    au_svg_doc_free(d);
    ST_CHECK(!au_svg_doc_new("<html/>", 7, 0), "a non-svg root is refused");
    ST_CHECK(!au_svg_doc_new("<svg xmlns='http://www.w3.org/2000/svg'/>", 41, 2), "depth limit");
}

static void test_text(void)
{
    if (!g_fonts) {
        printf("SKIP text: no font given\n");
        return;
    }
    AuSurface *s = render(SVG("<text x='10' y='30' font-size='20' fill='red'>HH</text>"
                              "<text x='190' y='30' font-size='20' text-anchor='end' fill='#00f'>HH</text>"
                              "<text x='100' y='60' font-size='20' text-anchor='middle'><tspan fill='lime'>H</tspan>"
                              "<tspan dy='10' fill='red'>H</tspan></text>"),
                          200, 80);
    int ink_l = 0, ink_r = 0;
    for (int y = 10; y < 32; y++) {
        for (int x = 0; x < 100; x++)
            ink_l += red(px(s, x, y)) > 128 && alpha(px(s, x, y)) > 128;
        for (int x = 100; x < 200; x++)
            ink_r += blue(px(s, x, y)) > 128 && alpha(px(s, x, y)) > 128;
    }
    ST_CHECK(ink_l > 40, "start-anchored text draws (%d)", ink_l);
    ST_CHECK(ink_r > 40, "end-anchored text draws left of x (%d)", ink_r);
    /* Ink right of 190 would mean the anchor was ignored. */
    int past = 0;
    for (int y = 10; y < 32; y++)
        for (int x = 192; x < 200; x++)
            past += alpha(px(s, x, y)) > 64;
    ST_CHECK(past == 0, "nothing past the end anchor (%d)", past);
    int g = 0, r = 0, gy = 0, ry = 0;
    for (int y = 35; y < 80; y++)
        for (int x = 70; x < 130; x++) {
            AuColor c = px(s, x, y);
            if (alpha(c) > 128 && green(c) > 128)
                g++, gy += y;
            if (alpha(c) > 128 && red(c) > 128)
                r++, ry += y;
        }
    ST_CHECK(g > 10 && r > 10, "tspans draw with their own fills (%d, %d)", g, r);
    if (g && r)
        ST_CHECK(ry / r - gy / g >= 8, "dy moves the second tspan down (%d vs %d)", ry / r, gy / g);
    au_surface_free(s);
}

static void test_image_pipeline(void)
{
    const char *svg = "  <svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 2 1'><rect width='1' height='1' fill='red'/>"
                      "<rect x='1' width='1' height='1' fill='#00f'/></svg>";
    ST_CHECK(au_image_sniff(svg, 16) == AU_IMAGE_FORMAT_SVG, "image sniffing finds SVG");
    AuImageInfo info;
    ST_CHECK(au_image_probe(svg, strlen(svg), &info) && info.width == 300 && info.height == 150 &&
                 info.format == AU_IMAGE_FORMAT_SVG,
             "probe: %dx%d", info.width, info.height);
    AuImageOptions o = {0};
    o.target_w = 600;
    AuSurface *s = au_image_decode_buffer(svg, strlen(svg), &o, &info);
    ST_CHECK(s && s->width == 600 && s->height == 300 && info.scale_denom == 1, "drawn at the target, larger");
    if (s) {
        EXPECT_PX(s, 100, 150, 255, 255, 0, 0);
        EXPECT_PX(s, 500, 150, 255, 0, 0, 255);
        au_surface_free(s);
    }
    /* A capped canvas keeps the ratio. */
    o.target_w = 20000;
    o.max_bytes = 4 * 1000 * 500;
    s = au_image_decode_buffer(svg, strlen(svg), &o, &info);
    ST_CHECK(s && s->width * s->height * 4 <= 4 * 1000 * 500 && abs(s->width - 2 * s->height) <= 2, "capped: %dx%d",
             s ? s->width : 0, s ? s->height : 0);
    au_surface_free(s);
    ST_CHECK(!au_image_decode_buffer("<html></html>", 13, NULL, NULL), "markup that is not SVG fails");

    /* The cache draws it again when shown larger, and keeps the bytes. */
    AuImageCache *c = au_image_cache_new(0);
    AuImageEntry *e = au_image_cache_add(c, "data:x");
    au_image_entry_data(c, e, svg, strlen(svg));
    au_image_entry_done(c, e, true);
    int w, h;
    ST_CHECK(au_image_entry_size(e, &w, &h) && w == 300 && h == 150, "cache size");
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 40, 20);
    const AuSurface *v = au_image_entry_surface(e);
    ST_CHECK(v->width == 40 && v->height == 20, "drawn at the display size: %dx%d", v->width, v->height);
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 800, 400);
    ST_CHECK(v->width == 800 && v->height == 400, "drawn again larger: %dx%d", v->width, v->height);
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 100, 100);
    ST_CHECK(v->width == 100 && v->height == 100, "drawn again for another shape: %dx%d", v->width, v->height);
    AuImageCacheStats cs;
    au_image_cache_stats(c, &cs);
    ST_CHECK(cs.compressed_bytes == strlen(svg), "SVG bytes kept");
    au_image_cache_free(c);
}

static void bench(void)
{
    /* 200 icons drawn into one canvas. */
    const char *icon = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path d='M12 2L2 7l10 5 10-5-10-5z"
                       "M2 17l10 5 10-5M2 12l10 5 10-5' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' "
                       "stroke-linejoin='round'/></svg>";
    AuSvgDoc *d = au_svg_doc_new(icon, strlen(icon), 0);
    AuSurface *s = au_surface_new(24 * 20, 24 * 10);
    AuCanvas *c = au_canvas_new(s);
    clock_t t0 = clock();
    for (int i = 0; i < 200; i++)
        au_svg_doc_render(d, c, (AuRect){(float)(i % 20) * 24, (float)(i / 20) * 24, 24, 24}, NULL);
    double ms = (double)(clock() - t0) * 1000 / CLOCKS_PER_SEC;
    uint32_t nodes;
    size_t bytes;
    au_svg_doc_stats(d, &nodes, &bytes);
    printf("bench: 200 icons in %.2f ms; icon document %u nodes, %zu bytes (tree and attributes)\n", ms, nodes, bytes);
    au_canvas_free(c);
    au_surface_free(s);
    au_svg_doc_free(d);
}

/* Inputs the fuzzer found. Each ran past a buffer or looped; here they
 * only have to render (the sanitizer build checks the memory). */
static void test_fuzz_regressions(void)
{
    /* preserveAspectRatio shorter than "xMidYMid": svg_parse_par read
     * s[4] past the end of the attribute's value. */
    static const char *const par[] = {"x", "xe", "xMi", "xMid", "xMidY", "", "defer", "defer x"};
    for (size_t i = 0; i < sizeof par / sizeof par[0]; i++) {
        char doc[256];
        snprintf(doc, sizeof doc,
                 "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8' viewBox='0 0 4 4' "
                 "preserveAspectRatio='%s'><rect width='4' height='4' fill='lime'/></svg>",
                 par[i]);
        AuSurface *s = render(doc, 8, 8);
        EXPECT_PX(s, 4, 4, 255, 0, 255, 0);
        au_surface_free(s);
    }
}

int main(int argc, char **argv)
{
    bool do_bench = false;
    uint8_t *font = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench")) {
            do_bench = true;
        } else {
            size_t n;
            font = st_read_file(argv[i], &n);
            if (font) {
                g_fonts = au_fontset_new();
                if (!au_fontset_add_data(g_fonts, font, n)) {
                    au_fontset_free(g_fonts);
                    g_fonts = NULL;
                }
            }
        }
    }
    test_path_grammar();
    test_transform_grammar();
    test_xml();
    test_sniff();
    test_shapes();
    test_viewbox();
    test_strokes();
    test_gradients();
    test_use();
    test_clip_mask();
    test_opacity_style();
    test_nested_switch_image();
    test_fuzz_regressions();
    test_intrinsic();
    test_text();
    test_image_pipeline();
    if (do_bench)
        bench();
    if (g_fonts)
        au_fontset_free(g_fonts);
    free(font);
    printf("%s (%d failures)\n", st_failures ? "FAILED" : "ok", st_failures);
    return st_failures ? 1 : 0;
}
