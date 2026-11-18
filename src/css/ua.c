/* The user agent style sheet and the presentational hints of HTML
 * attributes (HTML, "Rendering").
 *
 * The UA sheet follows the rendering section of the HTML standard, with
 * form controls given simple box styles that layout can draw. Hints are
 * built as declaration text and parsed like an inline style; the cascade
 * gives them author origin, the lowest author layer, and zero
 * specificity. */
#include "css_int.h"

#include <stdlib.h>

const char css_ua_sheet[] =
    "@namespace url(http://www.w3.org/1999/xhtml);\n"
    "@namespace svg url(http://www.w3.org/2000/svg);\n"
    /* SVG (Blink's svg.css): an inner svg clips; transforms of SVG
     * elements other than the outer svg turn about the user-space
     * origin. */
    "svg|svg:not(:root), svg|symbol, svg|image, svg|marker, svg|pattern { overflow: hidden; }\n"
    "svg|*:not(svg|svg), svg|svg svg|svg { transform-origin: 0 0; }\n"
    /* hidden elements */
    "area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template,"
    " title { display: none; }\n"
    "[hidden]:not([hidden=until-found i]):not(embed) { display: none; }\n"
    "embed[hidden] { display: inline; height: 0; width: 0; }\n"
    "input[type=hidden i] { display: none !important; }\n"
    "@media (scripting) { noscript { display: none !important; } }\n"
    /* Frame sets: rows or columns of equal size (the rows and cols
     * lengths are not read), each frame a nested document. */
    "frameset { display: flex; flex-direction: column; height: 100vh; margin: 0; }\n"
    "frameset[cols] { flex-direction: row; }\n"
    "frameset frameset { height: auto; }\n"
    "frame, frameset frameset { display: block; flex: 1 1 0; min-width: 0; min-height: 0; align-self: stretch; }\n"
    "frameset frameset { display: flex; }\n"
    /* the page */
    "html, body { display: block; }\n"
    "body { margin: 8px; }\n"
    /* flow content */
    "address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing,"
    " main, p, plaintext, pre, search, xmp { display: block; }\n"
    "blockquote, figure, listing, p, plaintext, pre, xmp { margin-block: 1em; }\n"
    "blockquote, figure { margin-inline: 40px; }\n"
    "address { font-style: italic; }\n"
    "listing, plaintext, pre, xmp { font-family: monospace; white-space: pre; }\n"
    "dialog:not([open]) { display: none; }\n"
    "dialog { position: absolute; inset-inline-start: 0; inset-inline-end: 0; width: fit-content;"
    " height: fit-content; margin: auto; border: solid; padding: 1em; background-color: Canvas;"
    " color: CanvasText; }\n"
    "dialog:modal { position: fixed; overflow: auto; inset-block: 0; max-width: calc(100% - 6px - 2em);"
    " max-height: calc(100% - 6px - 2em); }\n"
    "slot { display: contents; }\n"
    "pre[wrap] { white-space: pre-wrap; }\n"
    "center { text-align: -webkit-center; }\n"
    /* phrasing content */
    "cite, dfn, em, i, var { font-style: italic; }\n"
    "b, strong { font-weight: bolder; }\n"
    "code, kbd, samp, tt { font-family: monospace; }\n"
    "big { font-size: larger; }\n"
    "small { font-size: smaller; }\n"
    "sub { vertical-align: sub; }\n"
    "sup { vertical-align: super; }\n"
    "sub, sup { line-height: normal; font-size: smaller; }\n"
    "ruby { display: ruby; }\n"
    "rt { display: ruby-text; }\n"
    ":link { color: #0000EE; }\n"
    ":visited { color: #551A8B; }\n"
    ":link:active, :visited:active { color: #FF0000; }\n"
    ":link, :visited { text-decoration: underline; cursor: pointer; }\n"
    ":focus-visible { outline: auto; }\n"
    "mark { background: Mark; color: MarkText; }\n"
    "abbr[title], acronym[title] { text-decoration: dotted underline; }\n"
    "ins, u { text-decoration: underline; }\n"
    "del, s, strike { text-decoration: line-through; }\n"
    "q::before { content: open-quote; }\n"
    "q::after { content: close-quote; }\n"
    "nobr { white-space: nowrap; }\n"
    "nobr wbr { white-space: normal; }\n"
    /* bidi */
    "[dir]:dir(ltr), bdi:dir(ltr), input[type=tel i]:dir(ltr) { direction: ltr; }\n"
    "[dir]:dir(rtl), bdi:dir(rtl) { direction: rtl; }\n"
    "address, blockquote, center, div, figure, figcaption, footer, form, header, hr, legend, listing, main,"
    " p, plaintext, pre, search, summary, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section,"
    " table, caption, colgroup, col, thead, tbody, tfoot, tr, td, th, dir, dd, dl, dt, menu, ol, ul, li,"
    " bdi, output, [dir=ltr i], [dir=rtl i], [dir=auto i] { unicode-bidi: isolate; }\n"
    "bdo, bdo[dir] { unicode-bidi: isolate-override; }\n"
    "input[dir=auto i]:is([type=search i], [type=tel i], [type=url i], [type=email i]), textarea[dir=auto i],"
    " pre[dir=auto i] { unicode-bidi: plaintext; }\n"
    /* sections and headings */
    "article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section { display: block; }\n"
    "h1 { margin-block: 0.67em; font-size: 2em; }\n"
    "h2 { margin-block: 0.83em; font-size: 1.5em; }\n"
    "h3 { margin-block: 1em; font-size: 1.17em; }\n"
    "h4 { margin-block: 1.33em; font-size: 1em; }\n"
    "h5 { margin-block: 1.67em; font-size: 0.83em; }\n"
    "h6 { margin-block: 2.33em; font-size: 0.67em; }\n"
    "h1, h2, h3, h4, h5, h6 { font-weight: bold; }\n"
    ":is(article, aside, nav, section) h1 { margin-block: 0.83em; font-size: 1.5em; }\n"
    ":is(article, aside, nav, section) :is(article, aside, nav, section) h1 { margin-block: 1em;"
    " font-size: 1.17em; }\n"
    /* lists */
    "dir, dd, dl, dt, menu, ol, ul { display: block; }\n"
    "li { display: list-item; text-align: match-parent; }\n"
    "dir, dl, menu, ol, ul { margin-block: 1em; }\n"
    ":is(dir, dl, menu, ol, ul) :is(dir, dl, menu, ol, ul) { margin-block: 0; }\n"
    "dd { margin-inline-start: 40px; }\n"
    "dir, menu, ol, ul { padding-inline-start: 40px; }\n"
    "ol, ul, menu { counter-reset: list-item; }\n"
    "ol { list-style-type: decimal; }\n"
    "dir, menu, ul { list-style-type: disc; }\n"
    ":is(dir, menu, ol, ul) :is(dir, menu, ul) { list-style-type: circle; }\n"
    ":is(dir, menu, ol, ul) :is(dir, menu, ol, ul) :is(dir, menu, ul) { list-style-type: square; }\n"
    "::marker { unicode-bidi: isolate; white-space: pre; }\n"
    /* tables */
    "table { display: table; box-sizing: border-box; border-spacing: 2px; border-collapse: separate;"
    " text-indent: initial; border-color: gray; }\n"
    "caption { display: table-caption; text-align: center; }\n"
    "colgroup, colgroup[hidden] { display: table-column-group; }\n"
    "col, col[hidden] { display: table-column; }\n"
    "thead, thead[hidden] { display: table-header-group; }\n"
    "tbody, tbody[hidden] { display: table-row-group; }\n"
    "tfoot, tfoot[hidden] { display: table-footer-group; }\n"
    "tr, tr[hidden] { display: table-row; }\n"
    "td, th { display: table-cell; padding: 1px; }\n"
    "th { font-weight: bold; text-align: -webkit-center; }\n"
    "thead, tbody, tfoot, table > tr { vertical-align: middle; }\n"
    "tr, td, th { vertical-align: inherit; }\n"
    "thead, tbody, tfoot, tr { border-color: inherit; }\n"
    "td, th { border-color: gray; }\n"
    "table:is([rules=none i], [rules=groups i], [rules=rows i], [rules=cols i], [rules=all i]),"
    " table:is([frame=void i], [frame=above i], [frame=below i], [frame=hsides i], [frame=lhs i],"
    " [frame=rhs i], [frame=vsides i], [frame=box i], [frame=border i]) { border-style: hidden;"
    " border-collapse: collapse; }\n"
    "table[border] { border-style: outset; }\n"
    "table[border] > tr > :is(td, th), table[border] > :is(thead, tbody, tfoot) > tr > :is(td, th)"
    " { border-width: 1px; border-style: inset; }\n"
    "table[rules=all i] > tr > :is(td, th), table[rules=all i] > :is(thead, tbody, tfoot) > tr > :is(td, th)"
    " { border-width: 1px; border-style: solid; }\n"
    /* embedded content */
    "iframe { border: 2px inset; }\n"
    "video { object-fit: contain; }\n"
    /* hr and fieldset */
    "hr { color: gray; border-style: inset; border-width: 1px; margin-block: 0.5em; margin-inline: auto;"
    " overflow: hidden; }\n"
    "fieldset { display: block; margin-inline: 2px; border: groove 2px ThreeDFace;"
    " padding-block: 0.35em 0.625em; padding-inline: 0.75em; min-inline-size: min-content; }\n"
    "legend { padding-inline: 2px; }\n"
    /* details and summary */
    "details, summary { display: block; }\n"
    "details > summary:first-of-type { display: list-item; counter-increment: list-item 0;"
    " list-style: disclosure-closed inside; }\n"
    "details[open] > summary:first-of-type { list-style-type: disclosure-open; }\n"
    /* form controls */
    /* Form controls. The HTML rules come first; the box metrics follow
     * current browsers (Chrome's html.css: the same heights and
     * baselines), and layout paints Platinum-style controls over these
     * boxes while the author has not restyled them (appearance: auto and
     * no author background, border or box-shadow). */
    "input, select, button, textarea { letter-spacing: initial; word-spacing: initial; line-height: initial;"
    " text-transform: initial; text-indent: initial; text-shadow: initial; appearance: auto; }\n"
    "input, select, textarea { text-align: initial; }\n"
    "input:is([type=reset i], [type=button i], [type=submit i]), button { text-align: center; }\n"
    "input, button { display: inline-block; }\n"
    "input:is([type=radio i], [type=checkbox i], [type=reset i], [type=button i], [type=submit i],"
    " [type=color i], [type=search i]), select, button { box-sizing: border-box; }\n"
    "input, select, textarea, button { font: 13.333px system-ui, sans-serif; color: FieldText; margin: 0; }\n"
    "input { background-color: Field; border: 2px inset ButtonBorder; padding: 1px 2px; }\n"
    "input:is([type=reset i], [type=button i], [type=submit i]), button { background-color: ButtonFace;"
    " color: ButtonText; border: 2px outset ButtonBorder; padding: 1px 6px; }\n"
    "select { display: inline-block; background-color: ButtonFace; color: ButtonText;"
    " border: 1px solid ButtonBorder; padding: 1px 0 1px 4px; }\n"
    "select:is([multiple], [size]:not([size=\"0\"], [size=\"1\"])) { background-color: Field;"
    " color: FieldText; padding: 0; }\n"
    "input:is([type=checkbox i], [type=radio i]) { border: none; padding: 0; background-color: transparent;"
    " margin: 3px 3px 3px 4px; width: 13px; height: 13px; }\n"
    "input[type=radio i] { margin: 3px 3px 0 5px; }\n"
    "input[type=range i] { border: none; padding: 0; background-color: transparent; margin: 2px; }\n"
    "input[type=color i] { width: 50px; height: 27px; background-color: ButtonFace; border: 1px solid ButtonBorder;"
    " padding: 1px 2px; }\n"
    "input[type=file i] { border: none; padding: 0; background-color: transparent; }\n"
    "input[type=image i] { border: none; padding: 0; background-color: transparent; }\n"
    "textarea { display: inline-block; white-space: pre-wrap; overflow-wrap: break-word; font-family: monospace;"
    " background-color: Field; border: 1px solid ButtonBorder; padding: 2px; resize: both; overflow: auto; }\n"
    "option, optgroup { display: block; }\n"
    "optgroup { font-weight: bold; }\n"
    "select:not([multiple]):not([size]) option, select[size=\"1\"] option { display: none; }\n"
    "::placeholder { color: #757575; }\n"
    "button:disabled, input:disabled, select:disabled, textarea:disabled { color: GrayText; }\n"
    "progress, meter { appearance: auto; }\n"
    "progress { display: inline-block; width: 10em; height: 1em; vertical-align: -0.2em; }\n"
    "meter { display: inline-block; width: 5em; height: 1em; vertical-align: -0.2em; }\n"
    /* misc */
    "marquee { display: inline-block; text-align: initial; overflow: hidden !important; }\n"
    "[popover] { position: fixed; inset: 0; width: fit-content; height: fit-content; margin: auto;"
    " border: solid; padding: 0.25em; overflow: auto; color: CanvasText; background-color: Canvas; }\n"
    "[popover]:not(:popover-open):not(dialog[open]) { display: none; }\n"
    "dialog:popover-open { display: block; }\n"
    "::selection { background-color: Highlight; color: HighlightText; }\n"
    "::backdrop { position: fixed; inset: 0; }\n"
    "dialog::backdrop { background: rgba(0, 0, 0, 0.1); }\n"
    /* the page stays live around a popover (a press outside hides it) */
    "[popover]:popover-open::backdrop { pointer-events: none; }\n"
    "audio:not([controls]) { display: none !important; }\n"
    "audio { width: 300px; height: 54px; }\n"
    "video, canvas, img, iframe, embed, object { overflow: clip; }\n";

const char css_ua_quirks_sheet[] =
    "@namespace url(http://www.w3.org/1999/xhtml);\n"
    "img[align=left i] { margin-right: 3px; }\n"
    "img[align=right i] { margin-left: 3px; }\n"
    "li { list-style-position: inside; }\n"
    "li :is(dir, menu, ol, ul) { list-style-position: outside; }\n"
    ":is(dir, menu, ol, ul) :is(dir, menu, ol, ul, li) { list-style-position: outside; }\n"
    "table { font-weight: initial; font-style: initial; font-variant: initial; font-size: initial;"
    " line-height: initial; white-space: initial; text-align: initial; }\n"
    "input:not([type=image i]), textarea { box-sizing: border-box; }\n"
    "form { margin-block-end: 1em; }\n";

/* ---- Attribute parsing (HTML "common microsyntaxes") ---- */

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

/* Rules for parsing dimension values. Returns 0 on failure, 1 for px, 2
 * for a percentage. */
static int parse_dimension(const char *s, float *out)
{
    while (is_ws(*s))
        s++;
    if (*s < '0' || *s > '9')
        return 0;
    double v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') {
        s++;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * f;
            f /= 10;
        }
    }
    if (v > 1e7)
        v = 1e7;
    *out = (float)v;
    return *s == '%' ? 2 : 1;
}

static bool parse_nonneg_int(const char *s, long *out)
{
    while (is_ws(*s))
        s++;
    if (*s == '+')
        s++;
    if (*s < '0' || *s > '9')
        return false;
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v < 100000000)
            v = v * 10 + (*s - '0');
        s++;
    }
    *out = v;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Rules for parsing a legacy color value. */
static bool parse_legacy_color(const char *s, size_t n, AuColor *out)
{
    while (n && is_ws(*s)) {
        s++;
        n--;
    }
    while (n && is_ws(s[n - 1]))
        n--;
    if (!n)
        return false;
    char low[32];
    if (css_lower(s, n, low, sizeof low)) {
        if (strcmp(low, "transparent") == 0)
            return false;
        if (css_color_from_name(low, n, out))
            return true;
    }
    if (n == 4 && s[0] == '#' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0 && hexval(s[3]) >= 0) {
        *out = AU_COLOR_RGBA(hexval(s[1]) * 17, hexval(s[2]) * 17, hexval(s[3]) * 17, 255);
        return true;
    }
    /* the general algorithm, on code units (non-ASCII becomes "00") */
    char buf[128];
    size_t m = 0;
    for (size_t i = 0; i < n && m < sizeof buf - 2; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            /* skip the continuation bytes of this code point */
            while (i + 1 < n && ((unsigned char)s[i + 1] & 0xC0) == 0x80)
                i++;
            buf[m++] = '0';
            buf[m++] = '0';
        } else {
            buf[m++] = (char)c;
        }
    }
    size_t start = 0;
    if (m && buf[0] == '#')
        start = 1;
    for (size_t i = start; i < m; i++)
        if (hexval(buf[i]) < 0)
            buf[i] = '0';
    char *h = buf + start;
    size_t len = m - start;
    if (len > 128)
        len = 128;
    while (len == 0 || len % 3)
        h[len++] = '0';
    size_t comp = len / 3;
    const char *c[3] = {h, h + comp, h + 2 * comp};
    if (comp > 8) {
        size_t cut = comp - 8;
        for (int k = 0; k < 3; k++)
            c[k] += cut;
        comp = 8;
    }
    while (comp > 2 && c[0][0] == '0' && c[1][0] == '0' && c[2][0] == '0') {
        for (int k = 0; k < 3; k++)
            c[k]++;
        comp--;
    }
    int rgb[3];
    for (int k = 0; k < 3; k++) {
        if (comp >= 2)
            rgb[k] = hexval(c[k][0]) * 16 + hexval(c[k][1]);
        else
            rgb[k] = hexval(c[k][0]);
    }
    *out = AU_COLOR_RGBA(rgb[0], rgb[1], rgb[2], 255);
    return true;
}

static bool attr_ieq(const char *v, const char *lit)
{
    while (*v && *lit) {
        char a = *v++, b = *lit++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (a != b)
            return false;
    }
    return *v == 0 && *lit == 0;
}

/* ---- Building the hint text ---- */

static const char *get(const AuElement *el, int hn)
{
    const AuAttr *a = au_element_attr(el, css_hn[hn]);
    return a ? a->value : NULL;
}

static void put_color(AuBuf *b, const char *prop, const char *v)
{
    AuColor c;
    if (!v || !parse_legacy_color(v, strlen(v), &c))
        return;
    au_buf_printf(b, "%s:", prop);
    au_css_color_serialize(c, b);
    au_buf_putc(b, ';');
}

static void put_len(AuBuf *b, const char *prop, const char *v, bool allow_zero, bool allow_pct)
{
    float f;
    int k = v ? parse_dimension(v, &f) : 0;
    if (!k || (!allow_zero && f == 0) || (k == 2 && !allow_pct))
        return;
    au_buf_printf(b, "%s:", prop);
    css_serialize_number(f, b);
    au_buf_puts(b, k == 2 ? "%;" : "px;");
}

static void put_url(AuBuf *b, const char *prop, const char *v)
{
    if (!v)
        return;
    while (is_ws(*v))
        v++;
    if (!*v)
        return;
    au_buf_printf(b, "%s:url(", prop);
    css_serialize_string(v, strlen(v), b);
    au_buf_puts(b, ");");
}

static const AuElement *ancestor_table(const AuElement *el)
{
    for (const AuNode *p = el->node.parent; p && p->type == AU_NODE_ELEMENT; p = p->parent) {
        const AuElement *e = (const AuElement *)p;
        if (e->ns == AU_NS_HTML && e->tag == HN(table))
            return e;
    }
    return NULL;
}

static void put_align_text(AuBuf *b, const char *v, bool webkit)
{
    if (!v)
        return;
    if (attr_ieq(v, "left"))
        au_buf_puts(b, webkit ? "text-align:-webkit-left;" : "text-align:left;");
    else if (attr_ieq(v, "right"))
        au_buf_puts(b, webkit ? "text-align:-webkit-right;" : "text-align:right;");
    else if (attr_ieq(v, "center") || attr_ieq(v, "middle"))
        au_buf_puts(b, webkit ? "text-align:-webkit-center;" : "text-align:center;");
    else if (attr_ieq(v, "justify"))
        au_buf_puts(b, "text-align:justify;");
}

static void put_valign(AuBuf *b, const char *v)
{
    if (!v)
        return;
    if (attr_ieq(v, "top"))
        au_buf_puts(b, "vertical-align:top;");
    else if (attr_ieq(v, "middle") || attr_ieq(v, "center"))
        au_buf_puts(b, "vertical-align:middle;");
    else if (attr_ieq(v, "bottom"))
        au_buf_puts(b, "vertical-align:bottom;");
    else if (attr_ieq(v, "baseline"))
        au_buf_puts(b, "vertical-align:baseline;");
}

static void put_img_align(AuBuf *b, const char *v)
{
    if (!v)
        return;
    if (attr_ieq(v, "left"))
        au_buf_puts(b, "float:left;");
    else if (attr_ieq(v, "right"))
        au_buf_puts(b, "float:right;");
    else if (attr_ieq(v, "top"))
        au_buf_puts(b, "vertical-align:top;");
    else if (attr_ieq(v, "middle") || attr_ieq(v, "absmiddle") || attr_ieq(v, "abscenter"))
        au_buf_puts(b, "vertical-align:middle;");
    else if (attr_ieq(v, "bottom") || attr_ieq(v, "baseline"))
        au_buf_puts(b, "vertical-align:baseline;");
    else if (attr_ieq(v, "texttop"))
        au_buf_puts(b, "vertical-align:text-top;");
}

static void put_space(AuBuf *b, const AuElement *el)
{
    const char *h = get(el, CSS_HN_hspace), *v = get(el, CSS_HN_vspace);
    put_len(b, "margin-left", h, true, true);
    put_len(b, "margin-right", h, true, true);
    put_len(b, "margin-top", v, true, true);
    put_len(b, "margin-bottom", v, true, true);
}

static const char *const legacy_font_sizes[] = {"x-small", "small", "medium", "large", "x-large", "xx-large",
                                                "xxx-large"};

static void put_font_size(AuBuf *b, const char *v)
{
    while (is_ws(*v))
        v++;
    int mode = 0;
    if (*v == '+') {
        mode = 1;
        v++;
    } else if (*v == '-') {
        mode = -1;
        v++;
    }
    long n;
    if (!parse_nonneg_int(v, &n))
        return;
    long val = mode == 1 ? 3 + n : mode == -1 ? 3 - n : n;
    if (val > 7)
        val = 7;
    if (val < 1)
        val = 1;
    au_buf_printf(b, "font-size:%s;", legacy_font_sizes[val - 1]);
}

static void put_list_type(AuBuf *b, const char *v, bool ol)
{
    if (!v)
        return;
    const char *t = NULL;
    if (strcmp(v, "1") == 0)
        t = "decimal";
    else if (strcmp(v, "a") == 0)
        t = "lower-alpha";
    else if (strcmp(v, "A") == 0)
        t = "upper-alpha";
    else if (strcmp(v, "i") == 0)
        t = "lower-roman";
    else if (strcmp(v, "I") == 0)
        t = "upper-roman";
    else if (!ol && attr_ieq(v, "none"))
        t = "none";
    else if (!ol && attr_ieq(v, "disc"))
        t = "disc";
    else if (!ol && attr_ieq(v, "circle"))
        t = "circle";
    else if (!ol && attr_ieq(v, "square"))
        t = "square";
    if (t)
        au_buf_printf(b, "list-style-type:%s;", t);
}

static void put_font_face(AuBuf *b, const char *v)
{
    /* each comma-separated name as a string, so the attribute cannot
     * inject declarations */
    bool any = false;
    AuBuf tmp;
    au_buf_init(&tmp);
    while (*v) {
        while (is_ws(*v) || *v == ',')
            v++;
        const char *s = v;
        while (*v && *v != ',')
            v++;
        const char *e = v;
        while (e > s && is_ws(e[-1]))
            e--;
        if (e > s) {
            if (any)
                au_buf_putc(&tmp, ',');
            /* plain names stay identifiers; anything else is quoted */
            bool plain = !(*s >= '0' && *s <= '9') && *s != '-';
            for (const char *q = s; q < e && plain; q++) {
                unsigned char ch = (unsigned char)*q;
                plain = ch >= 0x80 || ch == ' ' || ch == '-' || ch == '_' || (ch >= '0' && ch <= '9') ||
                        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            }
            if (plain)
                au_buf_append(&tmp, s, (size_t)(e - s));
            else
                css_serialize_string(s, (size_t)(e - s), &tmp);
            any = true;
        }
    }
    if (any) {
        au_buf_puts(b, "font-family:");
        au_buf_append(b, tmp.data, tmp.len);
        au_buf_putc(b, ';');
    }
    au_buf_free(&tmp);
}

static void table_cell_hints(AuBuf *b, const AuElement *el, bool quirks)
{
    const AuElement *t = ancestor_table(el);
    if (t) {
        const char *cp = get(t, CSS_HN_cellpadding);
        float f;
        int k = cp ? parse_dimension(cp, &f) : 0;
        if (k == 1) {
            au_buf_puts(b, "padding:");
            css_serialize_number(f, b);
            au_buf_puts(b, "px;");
        }
        const char *bd = get(t, CSS_HN_border);
        long bw;
        if (bd && parse_nonneg_int(bd, &bw) && bw == 0 && !get(t, CSS_HN_rules))
            au_buf_puts(b, "border-width:0;");
    }
    put_len(b, "width", get(el, CSS_HN_width), false, true);
    put_len(b, "height", get(el, CSS_HN_height), false, true);
    if (get(el, CSS_HN_nowrap)) {
        /* quirk: nowrap is ignored if a fixed width is set */
        float f;
        const char *w = get(el, CSS_HN_width);
        if (!(quirks && w && parse_dimension(w, &f) == 1 && f > 0))
            au_buf_puts(b, "white-space:nowrap;");
    }
}

/* ---- SVG presentation attributes ----
 * SVG presentation attributes are presentational hints (author level,
 * below every author rule). The value is CSS text; attributes whose value
 * could close the declaration are skipped, and each becomes its own
 * declaration so one invalid value does not drop the others. Lengths
 * written as plain numbers are user units (px). width and height map only
 * on the svg element (the outer one sizes the CSS box). transform is read
 * by the SVG renderer itself (its attribute grammar differs from CSS). */
static const char *const svg_pres_attrs[] = {
    /* sorted for bsearch */
    "alignment-baseline", "baseline-shift", "clip-path", "clip-rule", "color", "color-interpolation",
    "cursor", "direction", "display", "dominant-baseline", "fill", "fill-opacity", "fill-rule", "filter",
    "flood-color", "flood-opacity", "font-family", "font-size", "font-size-adjust", "font-stretch",
    "font-style", "font-variant", "font-weight", "image-rendering", "letter-spacing", "lighting-color",
    "mask", "mask-type", "opacity", "overflow", "paint-order", "pointer-events", "shape-rendering",
    "stop-color", "stop-opacity", "stroke", "stroke-dasharray", "stroke-dashoffset", "stroke-linecap",
    "stroke-linejoin", "stroke-miterlimit", "stroke-opacity", "stroke-width", "text-anchor",
    "text-decoration", "text-rendering", "unicode-bidi", "vector-effect", "visibility", "word-spacing",
    "writing-mode"};

static int svg_attr_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, *(const char *const *)b);
}

static bool plain_number(const char *v)
{
    while (is_ws(*v))
        v++;
    if (*v == '+' || *v == '-')
        v++;
    bool digits = false;
    while (*v >= '0' && *v <= '9')
        v++, digits = true;
    if (*v == '.') {
        v++;
        while (*v >= '0' && *v <= '9')
            v++, digits = true;
    }
    if (digits && (*v == 'e' || *v == 'E')) {
        v++;
        if (*v == '+' || *v == '-')
            v++;
        if (!(*v >= '0' && *v <= '9'))
            return false;
        while (*v >= '0' && *v <= '9')
            v++;
    }
    while (is_ws(*v))
        v++;
    return digits && !*v;
}

static void svg_hints(const AuElement *el, AuBuf *b)
{
    bool outer_svg = el->tag == AU_ATOM_svg;
    for (uint32_t i = 0; i < el->nattrs; i++) {
        const AuAttr *a = &el->attrs[i];
        if (a->ns != AU_ATTR_NS_NONE || a->len > 4096)
            continue;
        const char *name = au_atom_str(a->name);
        bool size = outer_svg && (!strcmp(name, "width") || !strcmp(name, "height"));
        if (!size && !bsearch(name, svg_pres_attrs, CSS_ARRAY_LEN(svg_pres_attrs), sizeof svg_pres_attrs[0],
                              svg_attr_cmp))
            continue;
        const char *v = a->value;
        if (strpbrk(v, ";{}!\\\n\r"))
            continue;
        const char *p = v;
        while (is_ws(*p))
            p++;
        if (!*p)
            continue;
        au_buf_printf(b, "%s:%s", name, v);
        if ((size || !strcmp(name, "font-size") || !strcmp(name, "letter-spacing") ||
             !strcmp(name, "word-spacing") || !strcmp(name, "baseline-shift")) && plain_number(v))
            au_buf_puts(b, "px");
        au_buf_putc(b, ';');
    }
}

bool css_presentational_hints(const AuElement *el, AuCssDeclBlock *out, bool quirks)
{
    if (el->ns == AU_NS_SVG) {
        if (!el->nattrs)
            return false;
        AuBuf b;
        au_buf_init(&b);
        svg_hints(el, &b);
        bool any = false;
        if (b.len) {
            CssTokens tk = {0};
            css_tokenize(b.data, b.len, &tk);
            uint32_t before = out->n;
            const char *base = el->node.doc ? el->node.doc->url : NULL;
            css_parse_decl_list(&tk, 0, tk.n, out, false, base);
            css_tokens_free(&tk);
            any = out->n > before;
        }
        au_buf_free(&b);
        return any;
    }
    if (el->ns != AU_NS_HTML)
        return false;
    AuAtom tag = el->tag;
    /* Cells take hints from their table (cellpadding, border) even when
     * they have no attributes of their own. */
    if (!el->nattrs && tag != HN(td) && tag != HN(th))
        return false;
    AuBuf b;
    au_buf_init(&b);

    if (tag == HN(body)) {
        put_color(&b, "background-color", get(el, CSS_HN_bgcolor));
        put_url(&b, "background-image", get(el, CSS_HN_background));
        put_color(&b, "color", get(el, CSS_HN_text));
        static const char *const mattr[][2] = {{"marginheight", "margin-block"}, {"topmargin", "margin-top"},
                                               {"bottommargin", "margin-bottom"}, {"marginwidth", "margin-inline"},
                                               {"leftmargin", "margin-left"}, {"rightmargin", "margin-right"}};
        for (size_t i = 0; i < CSS_ARRAY_LEN(mattr); i++) {
            AuAtom a = au_atom_lookup(mattr[i][0], strlen(mattr[i][0]));
            const AuAttr *at = a ? au_element_attr(el, a) : NULL;
            if (at)
                put_len(&b, mattr[i][1], at->value, true, false);
        }
    } else if (tag == HN(table)) {
        put_color(&b, "background-color", get(el, CSS_HN_bgcolor));
        put_url(&b, "background-image", get(el, CSS_HN_background));
        put_len(&b, "width", get(el, CSS_HN_width), false, true);
        put_len(&b, "height", get(el, CSS_HN_height), true, true);
        const char *cs = get(el, CSS_HN_cellspacing);
        float f;
        if (cs && parse_dimension(cs, &f) == 1) {
            au_buf_puts(&b, "border-spacing:");
            css_serialize_number(f, &b);
            au_buf_puts(&b, "px;");
        }
        const char *al = get(el, CSS_HN_align);
        if (al && attr_ieq(al, "left"))
            au_buf_puts(&b, "float:left;");
        else if (al && attr_ieq(al, "right"))
            au_buf_puts(&b, "float:right;");
        else if (al && (attr_ieq(al, "center") || attr_ieq(al, "middle")))
            au_buf_puts(&b, "margin-inline:auto;");
        const char *bd = get(el, CSS_HN_border);
        long bw;
        if (bd) {
            if (!parse_nonneg_int(bd, &bw))
                bw = 1;
            au_buf_printf(&b, "border-width:%ldpx;", bw);
        }
        put_space(&b, el);
    } else if (tag == HN(td) || tag == HN(th)) {
        put_color(&b, "background-color", get(el, CSS_HN_bgcolor));
        put_url(&b, "background-image", get(el, CSS_HN_background));
        put_align_text(&b, get(el, CSS_HN_align), true);
        put_valign(&b, get(el, CSS_HN_valign));
        table_cell_hints(&b, el, quirks);
    } else if (tag == HN(tr) || tag == HN(thead) || tag == HN(tbody) || tag == HN(tfoot)) {
        put_color(&b, "background-color", get(el, CSS_HN_bgcolor));
        put_url(&b, "background-image", get(el, CSS_HN_background));
        put_align_text(&b, get(el, CSS_HN_align), true);
        put_valign(&b, get(el, CSS_HN_valign));
        if (tag == HN(tr))
            put_len(&b, "height", get(el, CSS_HN_height), false, true);
    } else if (tag == HN(col) || tag == HN(colgroup)) {
        put_len(&b, "width", get(el, CSS_HN_width), false, true);
        put_valign(&b, get(el, CSS_HN_valign));
    } else if (tag == HN(caption)) {
        put_align_text(&b, get(el, CSS_HN_align), false);
    } else if (tag == HN(div) || tag == HN(p) || tag == HN(h1) || tag == HN(h2) || tag == HN(h3) ||
               tag == HN(h4) || tag == HN(h5) || tag == HN(h6) || tag == HN(legend)) {
        put_align_text(&b, get(el, CSS_HN_align), tag == HN(div));
    } else if (tag == HN(img) || tag == HN(object) || tag == HN(embed) || tag == HN(iframe) ||
               tag == HN(video) || tag == HN(input)) {
        bool is_input = tag == HN(input);
        const char *ty = is_input ? get(el, CSS_HN_type) : NULL;
        if (!is_input || (ty && attr_ieq(ty, "image"))) {
            put_len(&b, "width", get(el, CSS_HN_width), true, true);
            put_len(&b, "height", get(el, CSS_HN_height), true, true);
            if (tag != HN(video))
                put_img_align(&b, get(el, CSS_HN_align));
            put_space(&b, el);
            if (tag == HN(img) || tag == HN(object) || is_input) {
                const char *bd = get(el, CSS_HN_border);
                long bw;
                if (bd && parse_nonneg_int(bd, &bw))
                    au_buf_printf(&b, "border:%ldpx solid;", bw);
            }
        }
        if (tag == HN(iframe)) {
            AuAtom fb = au_atom_lookup("frameborder", 11);
            const AuAttr *at = fb ? au_element_attr(el, fb) : NULL;
            if (at && (strcmp(at->value, "0") == 0 || attr_ieq(at->value, "no")))
                au_buf_puts(&b, "border:none;");
        }
    } else if (tag == HN(hr)) {
        const char *al = get(el, CSS_HN_align);
        if (al && attr_ieq(al, "left"))
            au_buf_puts(&b, "margin-left:0;margin-right:auto;");
        else if (al && attr_ieq(al, "right"))
            au_buf_puts(&b, "margin-left:auto;margin-right:0;");
        else if (al && attr_ieq(al, "center"))
            au_buf_puts(&b, "margin-left:auto;margin-right:auto;");
        const char *col = get(el, CSS_HN_color);
        AuColor c;
        if (col && parse_legacy_color(col, strlen(col), &c)) {
            au_buf_puts(&b, "border-style:solid;border-color:");
            au_css_color_serialize(c, &b);
            au_buf_puts(&b, ";background-color:");
            au_css_color_serialize(c, &b);
            au_buf_putc(&b, ';');
        } else if (get(el, CSS_HN_noshade)) {
            au_buf_puts(&b, "border-style:solid;border-color:gray;background-color:gray;");
        }
        const char *sz = get(el, CSS_HN_size);
        long n;
        if (sz && parse_nonneg_int(sz, &n) && n > 0) {
            if (n == 1)
                au_buf_puts(&b, "border-bottom-width:0;");
            else
                au_buf_printf(&b, "height:%ldpx;", n - 2 > 0 ? n - 2 : 0);
        }
        put_len(&b, "width", get(el, CSS_HN_width), false, true);
    } else if (tag == HN(font)) {
        put_color(&b, "color", get(el, CSS_HN_color));
        const char *face = get(el, CSS_HN_face);
        if (face)
            put_font_face(&b, face);
        const char *sz = get(el, CSS_HN_size);
        if (sz)
            put_font_size(&b, sz);
    } else if (tag == HN(ol) || tag == HN(ul) || tag == HN(li)) {
        put_list_type(&b, get(el, CSS_HN_type), tag == HN(ol));
    } else if (tag == HN(br)) {
        const char *cl = get(el, CSS_HN_clear);
        if (cl && (attr_ieq(cl, "left") || attr_ieq(cl, "right") || attr_ieq(cl, "both")))
            au_buf_printf(&b, "clear:%s;", attr_ieq(cl, "left") ? "left" : attr_ieq(cl, "right") ? "right" : "both");
        else if (cl && attr_ieq(cl, "all"))
            au_buf_puts(&b, "clear:both;");
    } else if (tag == HN(pre) || tag == HN(listing) || tag == HN(xmp)) {
        put_len(&b, "width", get(el, CSS_HN_width), false, false);
    } else if (tag == HN(marquee)) {
        put_color(&b, "background-color", get(el, CSS_HN_bgcolor));
        put_len(&b, "width", get(el, CSS_HN_width), true, true);
        put_len(&b, "height", get(el, CSS_HN_height), true, true);
        put_space(&b, el);
    }

    bool any = false;
    if (b.len) {
        CssTokens tk = {0};
        css_tokenize(b.data, b.len, &tk);
        uint32_t before = out->n;
        const char *base = el->node.doc ? el->node.doc->url : NULL;
        css_parse_decl_list(&tk, 0, tk.n, out, quirks, base);
        css_tokens_free(&tk);
        any = out->n > before;
    }
    au_buf_free(&b);
    return any;
}
