/* A small XML parser for SVG documents. See au_xml_parse in svg.h.
 *
 * One pass over the bytes with no recursion: the open elements are the
 * DOM's parent chain. Every read is bounded by the end of the input, and
 * the depth, node count, attribute count and name lengths are capped. No
 * DTD is read: the internal subset of a doctype is skipped unread, so no
 * entity can be declared and none expands except the five predefined ones
 * and character references (which rules out entity expansion attacks and
 * external entities alike). */
#include "svg_int.h"

#define NAME_MAX_LEN 256

typedef struct {
    const char *p, *end;
    AuDocument *doc;
    AuNode *cur; /* the open element, or the document */
    uint32_t depth, nodes;
    AuXmlLimits lim;
    AuBuf text;      /* character data waiting for the next markup */
    bool text_ws;    /* the waiting text is all whitespace */
    const char *err;
    bool root_done;  /* the root element has closed */
} X;

static bool fail(X *x, const char *msg)
{
    if (!x->err)
        x->err = msg;
    return false;
}

static bool starts(const X *x, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(x->end - x->p) >= n && memcmp(x->p, s, n) == 0;
}

static bool name_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80;
}

static bool name_char(unsigned char c)
{
    return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

/* Reads a name; returns its length (0 for none). */
static size_t read_name(X *x, const char **out)
{
    const char *s = x->p;
    if (s >= x->end || !name_start((unsigned char)*s))
        return 0;
    while (s < x->end && name_char((unsigned char)*s))
        s++;
    size_t n = (size_t)(s - x->p);
    if (n > NAME_MAX_LEN)
        return 0;
    *out = x->p;
    x->p = s;
    return n;
}

static void skip_ws(X *x)
{
    while (x->p < x->end && svg_ws(*x->p))
        x->p++;
}

static void put_utf8(AuBuf *b, uint32_t cp)
{
    char u[4];
    size_t n;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 63));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 63));
        u[2] = (char)(0x80 | (cp & 63));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 63));
        u[2] = (char)(0x80 | ((cp >> 6) & 63));
        u[3] = (char)(0x80 | (cp & 63));
        n = 4;
    }
    au_buf_append(b, u, n);
}

/* Decodes the reference at s (just after '&') within [s, end). Returns
 * the bytes consumed after the '&', or 0 to keep the '&' as written. */
static size_t entity(const char *s, const char *end, AuBuf *out)
{
    const char *semi = s;
    while (semi < end && semi - s < 12 && *semi != ';')
        semi++;
    if (semi >= end || *semi != ';')
        return 0;
    size_t n = (size_t)(semi - s);
    static const struct {
        const char *name;
        char c;
    } pre[] = {{"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'}};
    for (size_t i = 0; i < 5; i++)
        if (strlen(pre[i].name) == n && memcmp(s, pre[i].name, n) == 0) {
            au_buf_putc(out, pre[i].c);
            return n + 1;
        }
    if (n >= 2 && s[0] == '#') {
        uint32_t cp = 0;
        bool hex = s[1] == 'x';
        const char *q = s + (hex ? 2 : 1);
        if (q == semi)
            return 0;
        for (; q < semi; q++) {
            unsigned c = (unsigned char)*q, d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (hex && c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return 0;
            cp = cp * (hex ? 16 : 10) + d;
            if (cp > 0x10FFFF)
                cp = 0x110000;
        }
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFD;
        put_utf8(out, cp);
        return n + 1;
    }
    return 0;
}

/* Appends [s, e) to out with references decoded; attr: whitespace
 * characters become spaces (attribute value normalization). */
static void decode(const char *s, const char *e, AuBuf *out, bool attr)
{
    while (s < e) {
        const char *q = s;
        while (q < e && *q != '&' && !(attr && (*q == '\t' || *q == '\n' || *q == '\r')))
            q++;
        au_buf_append(out, s, (size_t)(q - s));
        if (q >= e)
            break;
        if (*q == '&') {
            size_t used = entity(q + 1, e, out);
            if (!used)
                au_buf_putc(out, '&');
            s = q + 1 + used;
        } else {
            au_buf_putc(out, ' ');
            s = q + 1;
        }
    }
}

static bool keep_ws_text(const AuNode *parent)
{
    if (!parent || parent->type != AU_NODE_ELEMENT)
        return false;
    const char *t = au_atom_str(((const AuElement *)parent)->tag);
    return !strcmp(t, "text") || !strcmp(t, "tspan") || !strcmp(t, "style") || !strcmp(t, "a") ||
           !strcmp(t, "title");
}

static bool flush_text(X *x)
{
    if (!x->text.len)
        return true;
    if (x->cur->type == AU_NODE_ELEMENT && (!x->text_ws || keep_ws_text(x->cur))) {
        if (++x->nodes > x->lim.max_nodes)
            return fail(x, "too many nodes");
        AuNode *t = au_text_new(x->doc, x->text.data, x->text.len);
        au_node_append(x->cur, t);
        au_node_unref(t);
    } else if (!x->text_ws && x->cur->type != AU_NODE_ELEMENT) {
        return fail(x, "text outside the root element");
    }
    x->text.len = 0;
    x->text_ws = true;
    return true;
}

static void add_text(X *x, const char *s, const char *e, bool raw)
{
    size_t before = x->text.len;
    if (raw)
        au_buf_append(&x->text, s, (size_t)(e - s));
    else
        decode(s, e, &x->text, false);
    for (size_t i = before; i < x->text.len && x->text_ws; i++)
        if (!svg_ws(x->text.data[i]))
            x->text_ws = false;
}

/* Skips to just after the terminator; false when it never comes. */
static bool skip_past(X *x, const char *term)
{
    size_t n = strlen(term);
    while ((size_t)(x->end - x->p) >= n) {
        if (memcmp(x->p, term, n) == 0) {
            x->p += n;
            return true;
        }
        x->p++;
    }
    return false;
}

/* <!DOCTYPE ...>, with any internal subset skipped unread. */
static bool skip_doctype(X *x)
{
    int bracket = 0;
    char quote = 0;
    while (x->p < x->end) {
        char c = *x->p++;
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[') {
            bracket++;
        } else if (c == ']') {
            if (bracket > 0)
                bracket--;
        } else if (c == '>' && bracket == 0) {
            return true;
        } else if (c == '<' && bracket > 0 && x->p < x->end && *x->p == '!' && x->end - x->p >= 3 &&
                   x->p[1] == '-' && x->p[2] == '-') {
            /* a comment inside the subset may hold brackets */
            if (!skip_past(x, "-->"))
                return false;
        }
    }
    return false;
}

/* The local name an element is created with: svg: prefixes drop. */
static AuAtom element_atom(const char *name, size_t n)
{
    if (n > 4 && memcmp(name, "svg:", 4) == 0)
        return au_atom(name + 4, n - 4);
    return au_atom(name, n);
}

static bool start_tag(X *x)
{
    x->p++; /* '<' */
    const char *name;
    size_t nlen = read_name(x, &name);
    if (!nlen)
        return fail(x, "bad element name");
    if (x->root_done)
        return fail(x, "content after the root element");
    if (x->depth >= x->lim.max_depth)
        return fail(x, "nesting too deep");
    if (++x->nodes > x->lim.max_nodes)
        return fail(x, "too many nodes");
    AuNode *n = au_element_new(x->doc, element_atom(name, nlen), AU_NS_SVG);
    AuElement *el = au_as_element(n);
    au_node_append(x->cur, n);
    au_node_unref(n);
    AuBuf val;
    au_buf_init(&val);
    bool self_close = false;
    for (;;) {
        const char *before = x->p;
        skip_ws(x);
        if (x->p >= x->end) {
            au_buf_free(&val);
            return fail(x, "end of input in a tag");
        }
        if (*x->p == '>') {
            x->p++;
            break;
        }
        if (*x->p == '/') {
            if (x->end - x->p < 2 || x->p[1] != '>') {
                au_buf_free(&val);
                return fail(x, "bad empty-element tag");
            }
            x->p += 2;
            self_close = true;
            break;
        }
        if (x->p == before) {
            au_buf_free(&val);
            return fail(x, "attributes need whitespace between them");
        }
        const char *an;
        size_t alen = read_name(x, &an);
        if (!alen) {
            au_buf_free(&val);
            return fail(x, "bad attribute name");
        }
        skip_ws(x);
        if (x->p >= x->end || *x->p != '=') {
            au_buf_free(&val);
            return fail(x, "attribute without a value");
        }
        x->p++;
        skip_ws(x);
        if (x->p >= x->end || (*x->p != '"' && *x->p != '\'')) {
            au_buf_free(&val);
            return fail(x, "unquoted attribute value");
        }
        char q = *x->p++;
        const char *vs = x->p;
        while (x->p < x->end && *x->p != q) {
            if (*x->p == '<') {
                au_buf_free(&val);
                return fail(x, "'<' in an attribute value");
            }
            x->p++;
        }
        if (x->p >= x->end) {
            au_buf_free(&val);
            return fail(x, "unterminated attribute value");
        }
        const char *ve = x->p++;
        if (el->nattrs >= x->lim.max_attrs) {
            au_buf_free(&val);
            return fail(x, "too many attributes");
        }
        /* Namespace declarations are not attributes the renderer reads. */
        if ((alen == 5 && memcmp(an, "xmlns", 5) == 0) || (alen > 6 && memcmp(an, "xmlns:", 6) == 0))
            continue;
        val.len = 0;
        decode(vs, ve, &val, true);
        AuAttrNamespace ns = AU_ATTR_NS_NONE;
        if (alen > 6 && memcmp(an, "xlink:", 6) == 0)
            ns = AU_ATTR_NS_XLINK;
        else if (alen > 4 && memcmp(an, "xml:", 4) == 0)
            ns = AU_ATTR_NS_XML;
        AuAtom aa = au_atom(an, alen);
        if (au_element_attr(el, aa)) {
            au_buf_free(&val);
            return fail(x, "duplicate attribute");
        }
        au_element_append_attr(el, aa, ns, val.data ? val.data : "", val.len);
    }
    au_buf_free(&val);
    if (!self_close) {
        x->cur = n;
        x->depth++;
    } else if (x->cur->type == AU_NODE_DOCUMENT) {
        x->root_done = true;
    }
    return true;
}

static bool end_tag(X *x)
{
    x->p += 2; /* "</" */
    const char *name;
    size_t nlen = read_name(x, &name);
    skip_ws(x);
    if (!nlen || x->p >= x->end || *x->p != '>')
        return fail(x, "bad end tag");
    x->p++;
    if (x->cur->type != AU_NODE_ELEMENT)
        return fail(x, "end tag without a start tag");
    AuAtom a = element_atom(name, nlen);
    if (au_as_element(x->cur)->tag != a)
        return fail(x, "mismatched end tag");
    x->cur = x->cur->parent;
    x->depth--;
    if (x->cur->type == AU_NODE_DOCUMENT)
        x->root_done = true;
    return true;
}

AuDocument *au_xml_parse(const char *data, size_t len, const AuXmlLimits *lim, const char **error)
{
    X x;
    memset(&x, 0, sizeof x);
    x.p = data;
    x.end = data + len;
    x.lim.max_depth = lim && lim->max_depth ? lim->max_depth : 256;
    x.lim.max_nodes = lim && lim->max_nodes ? lim->max_nodes : 100000;
    x.lim.max_attrs = lim && lim->max_attrs ? lim->max_attrs : 256;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        x.p += 3;
    x.doc = au_document_new();
    x.cur = &x.doc->node;
    au_buf_init(&x.text);
    x.text_ws = true;
    bool ok = true;
    while (ok && x.p < x.end) {
        if (*x.p != '<') {
            const char *s = x.p;
            while (x.p < x.end && *x.p != '<')
                x.p++;
            add_text(&x, s, x.p, false);
            continue;
        }
        if (starts(&x, "<!--")) {
            x.p += 4;
            ok = skip_past(&x, "-->") || fail(&x, "unterminated comment");
        } else if (starts(&x, "<![CDATA[")) {
            x.p += 9;
            const char *s = x.p;
            if (!skip_past(&x, "]]>")) {
                ok = fail(&x, "unterminated CDATA section");
                break;
            }
            if (x.cur->type != AU_NODE_ELEMENT) {
                ok = fail(&x, "CDATA outside the root element");
                break;
            }
            add_text(&x, s, x.p - 3, true);
        } else if (starts(&x, "<!DOCTYPE")) {
            if (x.cur->type != AU_NODE_DOCUMENT || x.root_done) {
                ok = fail(&x, "misplaced doctype");
                break;
            }
            x.p += 9;
            ok = skip_doctype(&x) || fail(&x, "unterminated doctype");
        } else if (starts(&x, "<?")) {
            x.p += 2;
            ok = skip_past(&x, "?>") || fail(&x, "unterminated processing instruction");
        } else if (starts(&x, "</")) {
            ok = flush_text(&x) && end_tag(&x);
        } else if (starts(&x, "<!")) {
            ok = fail(&x, "unknown markup declaration");
        } else {
            ok = flush_text(&x) && start_tag(&x);
        }
    }
    if (ok)
        ok = flush_text(&x);
    if (ok && x.cur != &x.doc->node)
        ok = fail(&x, "unclosed element");
    if (ok && !x.root_done)
        ok = fail(&x, "no root element");
    au_buf_free(&x.text);
    if (!ok) {
        if (error)
            *error = x.err ? x.err : "malformed";
        au_node_unref(&x.doc->node);
        return NULL;
    }
    if (error)
        *error = NULL;
    return x.doc;
}
