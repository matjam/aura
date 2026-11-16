/* Box tree generation from the styled DOM.
 *
 * One preorder walk over the DOM (an explicit stack, no recursion) builds
 * the boxes: element boxes by display type, text boxes with white space
 * collapsed and text-transform applied, ::before/::after/::marker boxes
 * with counters and quotes evaluated in document order, and replaced
 * boxes for images and form controls. A block-level box inside an inline
 * box splits the inline (CSS 2.1 9.2.1.1): the inline's ancestors up to
 * the block container are closed and continued after the block. A second
 * pass wraps inline runs in anonymous blocks wherever a block container
 * has block-level children, and wraps text in flex containers in
 * anonymous flex items. */
#include "layout_int.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- Value helpers ---- */

const AuCssValue *l_items(const AuCssValue *v, uint32_t *n)
{
    if (v->type == AU_CSSV_LIST || v->type == AU_CSSV_COMMA_LIST) {
        *n = v->n;
        return v->u.items;
    }
    *n = 1;
    return v;
}

const char *l_val_name(const AuCssValue *v, size_t *len)
{
    if (v->type == AU_CSSV_IDENT || v->type == AU_CSSV_STRING) {
        *len = v->n;
        return v->u.str;
    }
    if (v->type == AU_CSSV_KEYWORD) {
        const char *s = au_css_keyword_name((AuCssKeyword)v->kw);
        *len = strlen(s);
        return s;
    }
    *len = 0;
    return NULL;
}

static const char *attr_s(const AuNode *n, const char *name)
{
    if (!au_is_element(n))
        return NULL;
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr((const AuElement *)n, a) : NULL;
}

const char *l_attr(const LBox *b, AuAtom name)
{
    return au_is_element(b->node) ? au_element_get_attr((const AuElement *)b->node, name) : NULL;
}

bool l_is_tag(const LBox *b, AuAtom tag)
{
    return b->pseudo == 0 && au_is_html_element(b->node, tag);
}

/* ---- Builder ---- */

typedef struct Counter {
    const char *name;
    size_t nlen;
    int32_t value;
    uint32_t owner; /* frame depth whose closing ends the scope */
} Counter;

typedef struct BFrame {
    AuNode *dom;
    AuNode *child;
    LBox *box;            /* receives the children */
    const AuStyle *style; /* the element's style, for its text children */
    uint8_t stage;        /* 0 start, 1 children, 2 done */
    bool skip_children;
    bool out_of_flow;     /* restore last_space when it closes */
    bool saved_space;
    bool details_closed;  /* a closed <details>: only its first summary is rendered */
    bool summary_done;
} BFrame;

typedef struct Builder {
    AuLayout *L;
    BFrame *st;
    uint32_t n, cap;
    Counter *ctr;
    uint32_t nctr, ctr_cap;
    int quote_depth;
    bool last_space; /* the inline content so far ends in a collapsible space */
    AuBuf tmp;
} Builder;

static LBox *new_box(Builder *B, int kind, AuNode *node, const AuStyle *style, size_t size)
{
    LBox *b = ar_alloc(&B->L->tree, size);
    b->kind = (uint8_t)kind;
    b->node = node ? au_node_ref(node) : NULL;
    b->style = au_style_retain((AuStyle *)style);
    if (kind == LK_BLOCK) {
        LBlock *k = (LBlock *)b;
        k->baseline = k->last_baseline = NAN;
    }
    B->L->stats.boxes++;
    return b;
}

static void append(LBox *parent, LBox *c)
{
    c->parent = parent;
    c->next = NULL;
    if (parent->last)
        parent->last->next = c;
    else
        parent->first = c;
    parent->last = c;
}

static void insert_after(LBox *parent, LBox *after, LBox *c)
{
    c->parent = parent;
    if (!after) {
        c->next = parent->first;
        parent->first = c;
        if (!parent->last)
            parent->last = c;
        return;
    }
    c->next = after->next;
    after->next = c;
    if (parent->last == after)
        parent->last = c;
}

static bool is_block_level(const LBox *b)
{
    return b->kind == LK_BLOCK && !(b->flags & (LF_INLINE_LEVEL | LF_FLOAT | LF_ABSPOS | LF_MARKER_OUT));
}

static bool is_out_of_flow(const LBox *b)
{
    return b->kind == LK_BLOCK && (b->flags & (LF_FLOAT | LF_ABSPOS | LF_MARKER_OUT));
}

/* Inserts c into parent. A block-level box going into an inline box
 * splits the inline boxes up to the block container. */
/* Table-internal boxes: they go into an anonymous table in fixup. */
static bool is_table_internal(const LBox *b)
{
    if (b->kind != LK_BLOCK)
        return false;
    switch (b->inner) {
    case LI_TABLE_ROW_GROUP:
    case LI_TABLE_ROW:
    case LI_TABLE_CELL:
    case LI_TABLE_COLUMN:
    case LI_TABLE_COLUMN_GROUP: return true;
    default: return l_is_caption(b);
    }
}

static void insert_child(Builder *B, LBox *parent, LBox *c)
{
    /* Table parts in an inline box get an anonymous inline table there
     * (CSS Tables 3, fixup), so they do not split it. */
    if (parent->kind != LK_INLINE || !is_block_level(c) || is_table_internal(c)) {
        append(parent, c);
        return;
    }
    /* Chain of inline ancestors, outermost first. */
    LBox *chain[L_MAX_DEPTH];
    int n = 0;
    LBox *p = parent;
    while (p && p->kind == LK_INLINE && n < L_MAX_DEPTH) {
        chain[n++] = p;
        p = p->parent;
    }
    if (!p) {
        append(parent, c);
        return;
    }
    LBox *container = p;
    LBox *outer = chain[n - 1];
    insert_after(container, outer, c);
    /* Continuations, outermost first. */
    LBox *prev_clone = NULL;
    for (int i = n - 1; i >= 0; i--) {
        LBox *orig = chain[i];
        LBox *cl = new_box(B, LK_INLINE, orig->node, orig->style, sizeof(LBox));
        cl->pseudo = orig->pseudo;
        cl->flags = (orig->flags & ~LF_SPLIT_TAIL) | LF_SPLIT_HEAD;
        orig->flags |= LF_SPLIT_TAIL;
        if (prev_clone)
            append(prev_clone, cl);
        else
            insert_after(container, c, cl);
        prev_clone = cl;
        /* Frames that were adding to orig now add to its continuation. */
        for (uint32_t f = 0; f < B->n; f++)
            if (B->st[f].box == orig)
                B->st[f].box = cl;
    }
    B->L->stats.inlines += (uint32_t)n;
}

/* ---- Text ---- */

static uint32_t to_upper(uint32_t c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 32;
    if (c == 0xFF)
        return 0x178;
    if (c >= 0x100 && c <= 0x17F && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F)
        return (c & 1) == ((c >= 0x139 && c <= 0x148) || c >= 0x179 ? 0 : 1) ? c - 1 : c;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2)
        return c - 32;
    if (c >= 0x430 && c <= 0x44F)
        return c - 32;
    if (c >= 0x450 && c <= 0x45F)
        return c - 80;
    return c;
}

static uint32_t to_lower(uint32_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x138 && c != 0x149)
        return (c & 1) == ((c >= 0x139 && c <= 0x148) || c >= 0x179 ? 1 : 0) ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    return c;
}

static bool is_word_char(uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c != 0x2019);
}

/* Processes the text of one text node into out: text-transform, then
 * white space per white-space-collapse (CSS Text 3, 4.1). *last_space
 * says whether the inline content before it ends in a collapsible space,
 * and is updated. With map (n + 1 entries), map[i] receives the offset in
 * out of what source byte i became (the place before the character it
 * belongs to; map[n] is the length), never decreasing. */
void l_text_process(const AuStyle *s, const char *t, size_t n, bool *last_space, AuBuf *out, uint32_t *map)
{
    au_buf_clear(out);
    int collapse = s->inh->white_space_collapse;
    int transform = s->inhr->text_transform;
    bool prev_word = false;
    size_t i = 0;
    while (i < n) {
        size_t adv;
        uint32_t c = au_utf8_decode(t + i, n - i, &adv);
        if (map)
            for (size_t k = 0; k < adv && i + k < n; k++)
                map[i + k] = (uint32_t)out->len;
        i += adv;
        if (c == '\r') {
            if (i < n && t[i] == '\n')
                continue;
            c = '\n';
        }
        if (c == '\f')
            c = ' ';
        bool ws = c == ' ' || c == '\t' || c == '\n';
        if (ws && collapse == AU_WHITE_SPACE_COLLAPSE_COLLAPSE) {
            if (!*last_space)
                au_buf_putc(out, ' ');
            *last_space = true;
            prev_word = false;
            continue;
        }
        if (ws && collapse == AU_WHITE_SPACE_COLLAPSE_PRESERVE_BREAKS) {
            if (c == '\n') {
                /* Spaces before a preserved break are removed. */
                while (out->len && out->data[out->len - 1] == ' ')
                    out->len--;
                au_buf_putc(out, '\n');
                *last_space = true;
            } else if (!*last_space) {
                au_buf_putc(out, ' ');
                *last_space = true;
            }
            prev_word = false;
            continue;
        }
        if (collapse == AU_WHITE_SPACE_COLLAPSE_PRESERVE_SPACES && c == '\n')
            c = ' ';
        *last_space = false;
        switch (transform) {
        case AU_TEXT_TRANSFORM_UPPERCASE:
            if (c == 0xDF) {
                au_buf_puts(out, "SS");
                continue;
            }
            c = to_upper(c);
            break;
        case AU_TEXT_TRANSFORM_LOWERCASE:
            c = to_lower(c);
            break;
        case AU_TEXT_TRANSFORM_CAPITALIZE:
            if (is_word_char(c) && !prev_word && c != '\'')
                c = to_upper(c);
            break;
        case AU_TEXT_TRANSFORM_FULL_WIDTH:
            if (c > 0x20 && c < 0x7F)
                c += 0xFEE0;
            else if (c == ' ')
                c = 0x3000;
            break;
        default:
            break;
        }
        prev_word = is_word_char(c);
        if (ws)
            prev_word = false;
        au_buf_put_utf8(out, c);
    }
    if (map) {
        /* Spaces removed before a preserved break: keep the map monotonic. */
        map[n] = (uint32_t)out->len;
        for (size_t k = n; k-- > 0;)
            if (map[k] > map[k + 1])
                map[k] = map[k + 1];
    }
}

static void process_text(Builder *B, const AuStyle *s, const char *t, size_t n)
{
    l_text_process(s, t, n, &B->last_space, &B->tmp, NULL);
}

static void add_text(Builder *B, LBox *parent, AuNode *node, const AuStyle *s, const char *t, size_t n)
{
    process_text(B, s, t, n);
    if (!B->tmp.len)
        return;
    LText *tx = (LText *)new_box(B, LK_TEXT, node, s, sizeof(LText));
    tx->text = ar_strndup(&B->L->tree, B->tmp.data, B->tmp.len);
    tx->len = (uint32_t)B->tmp.len;
    B->L->stats.texts++;
    append(parent, &tx->b);
}

/* ---- Counters ---- */

static Counter *find_counter(Builder *B, const char *name, size_t len)
{
    for (uint32_t i = B->nctr; i-- > 0;)
        if (B->ctr[i].nlen == len && !memcmp(B->ctr[i].name, name, len))
            return &B->ctr[i];
    return NULL;
}

static Counter *new_counter(Builder *B, const char *name, size_t len, int32_t v)
{
    if (B->nctr == B->ctr_cap) {
        B->ctr_cap = B->ctr_cap ? B->ctr_cap * 2 : 32;
        B->ctr = au_realloc(B->ctr, B->ctr_cap * sizeof *B->ctr);
    }
    Counter *c = &B->ctr[B->nctr++];
    c->name = name;
    c->nlen = len;
    c->value = v;
    c->owner = B->n; /* the frame of the element's parent is st[n - 1]; its closing pops at depth n */
    return c;
}

static bool counter_mentions(const AuCssBlob *b, const char *name)
{
    if (!b)
        return false;
    uint32_t n;
    const AuCssValue *it = l_items(&b->value, &n);
    size_t nl = strlen(name);
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        size_t len;
        const char *s = l_val_name(&it[i], &len);
        if (s && len == nl && !memcmp(s, name, nl))
            return true;
    }
    return false;
}

static int32_t clamp_i(float v)
{
    if (v > 1e9f)
        return 1000000000;
    if (v < -1e9f)
        return -1000000000;
    return v != v ? 0 : (int32_t)v;
}

/* counter-reset, counter-increment, counter-set of one element or
 * pseudo-element, plus the implicit list-item increment. */
static void apply_counters(Builder *B, const AuStyle *s, AuNode *el, bool list_item, bool reversed_parent)
{
    const AuStyleRare *r = s->rare;
    uint32_t n;
    const AuCssValue *it;
    if (r->counter_reset && r->counter_reset->value.type == AU_CSSV_LIST) {
        it = l_items(&r->counter_reset->value, &n);
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            size_t len;
            const AuCssValue *nm = &it[i];
            bool rev = nm->type == AU_CSSV_FUNCTION && nm->n == 1;
            if (rev)
                nm = &nm->u.items[0];
            const char *name = l_val_name(nm, &len);
            if (!name)
                continue;
            int32_t v = clamp_i(it[i + 1].u.num);
            /* <ol start> and <ol reversed> feed the list-item reset. */
            if (len == 9 && !memcmp(name, "list-item", 9) && au_is_html_element(el, AU_ATOM_ol)) {
                const char *st = attr_s(el, "start");
                bool reversed = attr_s(el, "reversed") != NULL;
                int items = 0;
                if (reversed)
                    for (AuNode *c = el->first_child; c; c = c->next)
                        items += au_is_html_element(c, AU_ATOM_li);
                if (st && *st) {
                    /* Clamped so that the step below cannot overflow. */
                    long sv = strtol(st, NULL, 10);
                    if (sv > 1000000000L)
                        sv = 1000000000L;
                    if (sv < -1000000000L)
                        sv = -1000000000L;
                    v = (int32_t)sv + (reversed ? 1 : -1);
                } else if (reversed)
                    v = items + 1;
            }
            new_counter(B, name, len, v);
        }
    }
    bool li_done = false;
    if (r->counter_increment && r->counter_increment->value.type == AU_CSSV_LIST) {
        it = l_items(&r->counter_increment->value, &n);
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            size_t len;
            const char *name = l_val_name(&it[i], &len);
            if (!name)
                continue;
            if (len == 9 && !memcmp(name, "list-item", 9))
                li_done = true;
            Counter *c = find_counter(B, name, len);
            if (!c)
                c = new_counter(B, name, len, 0);
            c->value += clamp_i(it[i + 1].u.num);
        }
    }
    if (list_item && !li_done) {
        Counter *c = find_counter(B, "list-item", 9);
        if (!c)
            c = new_counter(B, "list-item", 9, 0);
        c->value += reversed_parent ? -1 : 1;
    }
    if (r->counter_set && r->counter_set->value.type == AU_CSSV_LIST) {
        it = l_items(&r->counter_set->value, &n);
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            size_t len;
            const char *name = l_val_name(&it[i], &len);
            if (!name)
                continue;
            Counter *c = find_counter(B, name, len);
            if (!c)
                c = new_counter(B, name, len, 0);
            c->value = clamp_i(it[i + 1].u.num);
        }
    }
    if (list_item && au_is_html_element(el, AU_ATOM_li) && !counter_mentions(r->counter_set, "list-item")) {
        const char *v = attr_s(el, "value");
        if (v && *v) {
            Counter *c = find_counter(B, "list-item", 9);
            if (c)
                c->value = (int32_t)strtol(v, NULL, 10);
        }
    }
}

/* Formats v in a counter style into out. Returns false for "none". */
static bool format_counter(AuBuf *out, int32_t v, const char *style, size_t slen)
{
#define IS(s) (slen == sizeof(s) - 1 && !memcmp(style, s, slen))
    if (!style || IS("decimal")) {
        au_buf_printf(out, "%ld", (long)v);
    } else if (IS("none")) {
        return false;
    } else if (IS("disc")) {
        au_buf_puts(out, "\xE2\x80\xA2");
    } else if (IS("circle")) {
        au_buf_puts(out, "\xE2\x97\xA6");
    } else if (IS("square")) {
        au_buf_puts(out, "\xE2\x96\xAA");
    } else if (IS("disclosure-open")) {
        au_buf_puts(out, "\xE2\x96\xBE");
    } else if (IS("disclosure-closed")) {
        au_buf_puts(out, "\xE2\x96\xB8");
    } else if (IS("decimal-leading-zero")) {
        au_buf_printf(out, v >= 0 && v < 10 ? "0%ld" : "%ld", (long)v);
    } else if ((IS("lower-roman") || IS("upper-roman")) && v > 0 && v < 4000) {
        static const char *const r[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
        static const int val[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
        bool up = style[0] == 'u';
        for (int i = 0; i < 13; i++)
            while (v >= val[i]) {
                for (const char *p = r[i]; *p; p++)
                    au_buf_putc(out, up ? (char)(*p - 32) : *p);
                v -= val[i];
            }
    } else if ((IS("lower-alpha") || IS("lower-latin") || IS("upper-alpha") || IS("upper-latin")) && v > 0) {
        char buf[16];
        int n = 0;
        char base = style[0] == 'u' ? 'A' : 'a';
        while (v > 0 && n < 15) {
            v--;
            buf[n++] = (char)(base + v % 26);
            v /= 26;
        }
        while (n)
            au_buf_putc(out, buf[--n]);
    } else if (IS("lower-greek") && v > 0) {
        char buf[64];
        int n = 0;
        while (v > 0 && n < 60) {
            v--;
            uint32_t cp = 0x3B1 + (uint32_t)(v % 24);
            if (cp >= 0x3C2)
                cp++;
            buf[n++] = (char)(0x80 | (cp & 0x3F));
            buf[n++] = (char)(0xC0 | (cp >> 6));
            v /= 24;
        }
        while (n) {
            au_buf_putc(out, buf[n - 1]);
            au_buf_putc(out, buf[n - 2]);
            n -= 2;
        }
    } else {
        au_buf_printf(out, "%ld", (long)v);
    }
    return true;
#undef IS
}

/* ---- Generated content ---- */

static const char *quote_str(const AuStyle *s, int depth, bool open)
{
    const AuCssBlob *q = s->inhr->quotes;
    if (q && q->value.type == AU_CSSV_KEYWORD && q->value.kw == AU_K_none)
        return "";
    if (q && q->value.type == AU_CSSV_LIST && q->value.n >= 2) {
        uint32_t pairs = q->value.n / 2;
        uint32_t d = depth < 0 ? 0 : (uint32_t)depth;
        if (d >= pairs)
            d = pairs - 1;
        return q->value.u.items[d * 2 + (open ? 0 : 1)].u.str;
    }
    if (depth == 0)
        return open ? "\xE2\x80\x9C" : "\xE2\x80\x9D";
    return open ? "\xE2\x80\x98" : "\xE2\x80\x99";
}

static LBox *make_element_box(Builder *B, AuNode *node, const AuStyle *s, int pseudo, bool *skip_children);

static void resolve_url(AuLayout *L, const char *url, char *out, size_t cap)
{
    if (!L->env.resolve_url || !L->env.resolve_url(L->env.ctx, url, out, cap)) {
        size_t n = strlen(url);
        if (n >= cap)
            n = cap - 1;
        memcpy(out, url, n);
        out[n] = 0;
    }
}

bool l_style_contains_fixed(const AuStyle *s)
{
    const AuStyleRare *r = s->rare;
    if (r->transform || r->translate || r->rotate || r->scale || r->filter || r->backdrop_filter ||
        r->perspective.kind == AU_LEN_PX || (r->contain & (AU_CONTAIN_LAYOUT | AU_CONTAIN_PAINT)))
        return true;
    if (r->will_change) {
        uint32_t n;
        const AuCssValue *it = l_items(&r->will_change->value, &n);
        for (uint32_t i = 0; i < n; i++) {
            const char *name = it[i].type == AU_CSSV_IDENT ? it[i].u.str : NULL;
            if (name && (!strcmp(name, "transform") || !strcmp(name, "perspective") || !strcmp(name, "filter") ||
                         !strcmp(name, "translate") || !strcmp(name, "rotate") || !strcmp(name, "scale") ||
                         !strcmp(name, "backdrop-filter") || !strcmp(name, "contain")))
                return true;
        }
    }
    return false;
}

int l_image_refresh(AuLayout *L, LReplaced *rep)
{
    if (!rep->src)
        return 0;
    int w = 0, h = 0;
    const AuSurface *px = NULL;
    bool loaded = L->env.image && L->env.image(L->env.ctx, rep->src, &w, &h, &px);
    if (!loaded) {
        w = h = 0;
        px = NULL;
    }
    float d = rep->density > 0 ? rep->density : 1;
    float iw = (float)w / d, ih = (float)h / d;
    int r = 0;
    if (loaded != rep->loaded || iw != rep->iw || ih != rep->ih)
        r = 2;
    else if (px != rep->pixels)
        r = 1;
    rep->loaded = loaded;
    rep->iw = iw;
    rep->ih = ih;
    rep->ratio = h > 0 ? (float)w / h : 0;
    rep->pixels = px;
    rep->url = loaded && L->env.image_use ? rep->src : NULL;
    rep->label = loaded ? NULL : rep->alt;
    return r;
}

static void image_lookup(AuLayout *L, LReplaced *rep, const char *url)
{
    char abs[2048];
    if (!url || !*url)
        return;
    resolve_url(L, url, abs, sizeof abs);
    rep->src = ar_strndup(&L->tree, abs, strlen(abs));
    l_image_refresh(L, rep);
}

/* ---- Image sources: srcset, sizes and <picture> (HTML, "Images") ---- */

static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* A source size value (a CSS length) in pixels, or -1. */
static float source_length(const char *p, const char *end, float vw, float vh)
{
    char buf[64];
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= sizeof buf)
        return -1;
    memcpy(buf, p, n);
    buf[n] = 0;
    char *u;
    double v = strtod(buf, &u);
    if (u == buf || v < 0)
        return -1;
    while (*u && ws(*u))
        u++;
    size_t ul = strlen(u);
    while (ul && ws(u[ul - 1]))
        ul--;
    if (ul == 2 && au_ieq(u, 2, "px", 2))
        return (float)v;
    if (ul == 2 && au_ieq(u, 2, "vw", 2))
        return (float)v * vw / 100;
    if (ul == 2 && au_ieq(u, 2, "vh", 2))
        return (float)v * vh / 100;
    if ((ul == 2 && au_ieq(u, 2, "em", 2)) || (ul == 3 && au_ieq(u, 3, "rem", 3)))
        return (float)v * 16;
    if (ul == 0 && v == 0)
        return 0;
    return -1;
}

/* The source size of a sizes attribute: the length of the first entry
 * whose media condition matches, else the last entry's, else 100vw. */
static float source_size(const char *sizes, float vw, float vh)
{
    if (!sizes)
        return vw;
    AuCssDevice dev;
    au_css_device_default(&dev);
    dev.width = vw;
    dev.height = vh;
    const char *p = sizes;
    while (*p) {
        const char *e = p;
        int depth = 0;
        while (*e && (*e != ',' || depth > 0)) {
            if (*e == '(')
                depth++;
            else if (*e == ')' && depth > 0)
                depth--;
            e++;
        }
        const char *a = p, *b = e;
        while (a < b && ws(*a))
            a++;
        while (b > a && ws(b[-1]))
            b--;
        /* The length is the last token; a media condition ends with ')'. */
        const char *len = b;
        while (len > a && !ws(len[-1]) && len[-1] != ')')
            len--;
        const char *cond_end = len;
        while (cond_end > a && ws(cond_end[-1]))
            cond_end--;
        float px = source_length(len, b, vw, vh);
        if (px >= 0 && (cond_end == a || au_css_media_text_matches(a, (size_t)(cond_end - a), &dev)))
            return px;
        p = *e ? e + 1 : e;
    }
    return vw;
}

/* Picks the candidate of a srcset attribute for a device of one device
 * pixel per CSS pixel: the lowest density that is at least 1, else the
 * highest (as Blink picks for its device pixel ratio). src, when given,
 * is the 1x candidate of a list without width descriptors. Returns the
 * URL's start and sets *len; *viewport is set when the choice depends on
 * the viewport (width descriptors). */
static const char *pick_candidate(const char *srcset, const char *sizes, const char *src, float vw, float vh,
                                  size_t *len, bool *viewport, float *density)
{
    const char *best = NULL;
    size_t best_len = 0;
    float best_d = 0;
    bool any_w = false;
    float size = -1;
    const char *p = srcset ? srcset : "";
    while (*p) {
        while (*p && (ws(*p) || *p == ','))
            p++;
        if (!*p)
            break;
        const char *u = p;
        while (*p && !ws(*p))
            p++;
        const char *ue = p;
        bool comma_ended = false;
        while (ue > u && ue[-1] == ',') {
            ue--;
            comma_ended = true;
        }
        float d = 1;
        bool bad = false;
        while (!comma_ended) {
            while (*p && ws(*p))
                p++;
            if (!*p)
                break;
            if (*p == ',') {
                p++;
                break;
            }
            const char *t = p;
            while (*p && !ws(*p) && *p != ',')
                p++;
            char num[32];
            size_t tl = (size_t)(p - t);
            if (tl < 2 || tl > sizeof num) {
                bad = true;
                continue;
            }
            memcpy(num, t, tl - 1);
            num[tl - 1] = 0;
            char *ne;
            double v = strtod(num, &ne);
            char unit = t[tl - 1];
            if (*ne || v <= 0)
                bad = true;
            else if (unit == 'x')
                d = (float)v;
            else if (unit == 'w') {
                if (size < 0)
                    size = source_size(sizes, vw, vh);
                any_w = true;
                d = size > 0 ? (float)v / size : 1e9f;
            } else if (unit != 'h')
                bad = true;
        }
        if (bad || ue == u)
            continue;
        if (!best || (d >= 1 && (best_d < 1 || d < best_d)) || (d < 1 && best_d < 1 && d > best_d)) {
            best = u;
            best_len = (size_t)(ue - u);
            best_d = d;
        }
    }
    if (any_w)
        *viewport = true;
    if (src && *src && !any_w && (!best || best_d != 1)) {
        /* src is a 1x candidate; an exact 1x in the list comes first. */
        best = src;
        best_len = strlen(src);
        best_d = 1;
    }
    *len = best_len;
    if (best)
        *density = best_d;
    return best;
}

static bool image_type_supported(const char *type)
{
    static const char *const ok[] = {"image/png",  "image/jpeg", "image/gif",    "image/webp",  "image/bmp",
                                     "image/apng", "image/x-icon", "image/svg+xml", "image/vnd.microsoft.icon"};
    size_t n = 0;
    while (type[n] && type[n] != ';' && !ws(type[n]))
        n++;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (au_ieq(type, n, ok[i], strlen(ok[i])))
            return true;
    return false;
}

/* The URL an <img> shows at a vw x vh viewport, as written (not
 * resolved): from the first usable <source> of its <picture> parent,
 * else its srcset and src. Returns NULL when it has none. */
static const char *img_source(const AuNode *img, float vw, float vh, size_t *len, bool *viewport, float *density)
{
    *density = 1;
    const AuNode *pic = img->parent;
    if (pic && au_is_html_element(pic, AU_ATOM_picture)) {
        AuCssDevice dev;
        au_css_device_default(&dev);
        dev.width = vw;
        dev.height = vh;
        for (const AuNode *c = pic->first_child; c && c != img; c = c->next) {
            if (!au_is_html_element(c, AU_ATOM_source))
                continue;
            const char *set = attr_s(c, "srcset");
            if (!set || !*set)
                continue;
            const char *media = attr_s(c, "media"), *type = attr_s(c, "type");
            if (media && *media) {
                *viewport = true;
                if (!au_css_media_text_matches(media, strlen(media), &dev))
                    continue;
            }
            if (type && *type && !image_type_supported(type))
                continue;
            const char *u = pick_candidate(set, attr_s(c, "sizes"), NULL, vw, vh, len, viewport, density);
            if (u)
                return u;
        }
    }
    const char *src = attr_s(img, "src");
    const char *set = attr_s(img, "srcset");
    if (set && *set) {
        const char *u = pick_candidate(set, attr_s(img, "sizes"), src, vw, vh, len, viewport, density);
        if (u)
            return u;
    }
    *density = 1;
    *len = src ? strlen(src) : 0;
    return src;
}

bool au_layout_image_source(AuLayout *L, const AuElement *el, AuBuf *out)
{
    size_t len = 0;
    bool viewport = false;
    float density;
    const char *u;
    if (au_is_html_element(&el->node, AU_ATOM_img))
        u = img_source(&el->node, L->src_vw > 0 ? L->src_vw : 1024, L->src_vh > 0 ? L->src_vh : 768, &len, &viewport,
                       &density);
    else {
        u = attr_s(&el->node, "src");
        len = u ? strlen(u) : 0;
    }
    if (!u || !len)
        return false;
    au_buf_append(out, u, len);
    return true;
}

/* Content items of a ::before/::after/::marker style into box. */
static void add_content(Builder *B, LBox *box, AuNode *el, const AuStyle *ps)
{
    const AuCssBlob *cb = ps->rare->content;
    if (!cb)
        return;
    uint32_t n;
    const AuCssValue *it = l_items(&cb->value, &n);
    AuBuf text;
    au_buf_init(&text);
    for (uint32_t i = 0; i < n; i++) {
        const AuCssValue *v = &it[i];
        if (v->type == AU_CSSV_DELIM)
            break; /* alt text follows */
        if (v->type == AU_CSSV_STRING) {
            au_buf_append(&text, v->u.str, v->n);
        } else if (v->type == AU_CSSV_KEYWORD) {
            switch (v->kw) {
            case AU_K_open_quote:
                au_buf_puts(&text, quote_str(ps, B->quote_depth, true));
                B->quote_depth++;
                break;
            case AU_K_close_quote:
                if (B->quote_depth > 0)
                    B->quote_depth--;
                au_buf_puts(&text, quote_str(ps, B->quote_depth, false));
                break;
            case AU_K_no_open_quote:
                B->quote_depth++;
                break;
            case AU_K_no_close_quote:
                if (B->quote_depth > 0)
                    B->quote_depth--;
                break;
            default:
                break;
            }
        } else if (v->type == AU_CSSV_FUNCTION && (v->kw == AU_K_counter || v->kw == AU_K_counters)) {
            size_t nl;
            const char *name = l_val_name(&v->u.items[0], &nl);
            const AuCssValue *stv = NULL;
            const char *sep = NULL;
            if (v->kw == AU_K_counter && v->n == 2)
                stv = &v->u.items[1];
            if (v->kw == AU_K_counters) {
                sep = v->u.items[1].u.str;
                if (v->n == 3)
                    stv = &v->u.items[2];
            }
            size_t sl = 0;
            const char *style = stv ? l_val_name(stv, &sl) : NULL;
            if (!name)
                continue;
            if (sep) {
                bool any = false;
                for (uint32_t c = 0; c < B->nctr; c++)
                    if (B->ctr[c].nlen == nl && !memcmp(B->ctr[c].name, name, nl)) {
                        if (any)
                            au_buf_puts(&text, sep);
                        format_counter(&text, B->ctr[c].value, style, sl);
                        any = true;
                    }
                if (!any)
                    format_counter(&text, 0, style, sl);
            } else {
                Counter *c = find_counter(B, name, nl);
                format_counter(&text, c ? c->value : 0, style, sl);
            }
        } else if (v->type == AU_CSSV_URL) {
            if (text.len) {
                add_text(B, box, el, ps, text.data, text.len);
                au_buf_clear(&text);
            }
            LBlock *img = (LBlock *)new_box(B, LK_BLOCK, el, ps, sizeof(LBlock));
            img->b.inner = LI_REPLACED;
            img->b.replaced = LR_IMAGE;
            img->b.flags = LF_ANON | LF_INLINE_LEVEL;
            img->b.pseudo = box->pseudo;
            img->rep = ar_alloc(&B->L->tree, sizeof *img->rep);
            image_lookup(B->L, img->rep, v->u.str);
            append(box, &img->b);
            B->last_space = false;
            B->L->stats.blocks++;
        }
    }
    if (text.len)
        add_text(B, box, el, ps, text.data, text.len);
    au_buf_free(&text);
}

static void make_pseudo(Builder *B, LBox *parent, AuNode *el, const AuStyle *es, AuCssPseudo which)
{
    const AuStyle *ps = au_style_pseudo(es, which);
    if (!ps || !ps->rare->content)
        return;
    const AuCssValue *cv = &ps->rare->content->value;
    if (cv->type == AU_CSSV_KEYWORD && (cv->kw == AU_K_none || cv->kw == AU_K_normal))
        return;
    if (ps->box->display == AU_DISPLAY_NONE)
        return;
    apply_counters(B, ps, el, false, false);
    if (ps->box->display == AU_DISPLAY_CONTENTS) {
        add_content(B, parent, el, ps);
        return;
    }
    bool skip;
    LBox *box = make_element_box(B, el, ps, which, &skip);
    insert_child(B, parent, box);
    if (box->kind == LK_BLOCK && !(box->flags & LF_INLINE_LEVEL))
        B->last_space = true;
    add_content(B, box, el, ps);
    if (box->kind == LK_BLOCK)
        B->last_space = box->flags & LF_INLINE_LEVEL ? false : true;
}

/* The list marker of a list item. */
static void make_marker(Builder *B, LBox *item, AuNode *el, const AuStyle *es)
{
    const AuStyle *ms = au_style_pseudo(es, AU_CSS_PSEUDO_MARKER);
    const AuStyle *s = ms ? ms : es;
    bool inside = es->inh->list_style_position == AU_LIST_STYLE_POSITION_INSIDE;
    AuBuf text;
    au_buf_init(&text);
    int shape = 0;
    if (ms && ms->rare->content && ms->rare->content->value.type != AU_CSSV_KEYWORD) {
        /* ::marker { content: ... } */
        if (!inside) {
            LBlock *blk = (LBlock *)new_box(B, LK_BLOCK, el, s, sizeof(LBlock));
            blk->b.pseudo = AU_CSS_PSEUDO_MARKER;
            blk->b.flags = LF_MARKER_OUT | LF_BFC | LF_ANON;
            blk->b.inner = LI_FLOW;
            add_content(B, &blk->b, el, ms);
            append(item, &blk->b);
            B->L->stats.blocks++;
            au_buf_free(&text);
            return;
        }
        LBox *box = new_box(B, LK_INLINE, el, s, sizeof(LBox));
        box->pseudo = AU_CSS_PSEUDO_MARKER;
        add_content(B, box, el, ms);
        append(item, box);
        B->L->stats.inlines++;
        au_buf_free(&text);
        return;
    }
    /* list-style-image: the image is the marker while the loader knows
     * it; a failed image falls back to list-style-type (CSS Lists 3,
     * section 3.2). */
    const AuCssBlob *li = es->inhr->list_style_image;
    if (li && li->value.type == AU_CSSV_URL && li->value.u.str && *li->value.u.str) {
        LReplaced *rep = ar_alloc(&B->L->tree, sizeof *rep);
        image_lookup(B->L, rep, li->value.u.str);
        if (rep->loaded) {
            /* Not sized yet, or failing later: the marker may become the
             * list-style-type one, which is another box. */
            if (rep->iw <= 0 || rep->ih <= 0)
                B->L->images_shape = true;
            rep->marker = true;
            LBlock *blk = (LBlock *)new_box(B, LK_BLOCK, el, s, sizeof(LBlock));
            blk->b.pseudo = AU_CSS_PSEUDO_MARKER;
            blk->b.inner = LI_REPLACED;
            blk->b.replaced = LR_IMAGE;
            blk->b.flags = LF_ANON | (inside ? LF_INLINE_LEVEL : LF_MARKER_OUT);
            blk->rep = rep;
            append(item, &blk->b);
            B->L->stats.blocks++;
            if (inside)
                B->last_space = false;
            au_buf_free(&text);
            return;
        }
    }
    const AuCssBlob *lt = es->inh->list_style_type;
    Counter *c = find_counter(B, "list-item", 9);
    int32_t v = c ? c->value : 1;
    bool have = false;
    if (lt && lt->value.type == AU_CSSV_STRING) {
        au_buf_append(&text, lt->value.u.str, lt->value.n);
        have = true;
    } else {
        size_t sl = 7;
        const char *st = lt ? l_val_name(&lt->value, &sl) : "decimal";
        if (!st) {
            st = "decimal";
            sl = 7;
        }
        if (sl == 4 && !memcmp(st, "disc", 4))
            shape = LR_DISC;
        else if (sl == 6 && !memcmp(st, "circle", 6))
            shape = LR_CIRCLE;
        else if (sl == 6 && !memcmp(st, "square", 6))
            shape = LR_SQUARE;
        else if (sl == 15 && !memcmp(st, "disclosure-open", 15))
            shape = LR_DISCLOSURE_OPEN; /* drawn: the triangles are missing from many fonts */
        else if (sl == 17 && !memcmp(st, "disclosure-closed", 17))
            shape = LR_DISCLOSURE_CLOSED;
        else if (format_counter(&text, v, st, sl)) {
            /* The suffix space is a no-break space so that it survives
             * white space processing (::marker has white-space: pre). */
            if (!(sl >= 10 && !memcmp(st, "disclosure", 10)))
                au_buf_puts(&text, ".\xC2\xA0");
            else
                au_buf_puts(&text, "\xC2\xA0");
            have = true;
        }
    }
    if (!have && !shape) {
        au_buf_free(&text);
        return;
    }
    if (shape) {
        LBlock *blk = (LBlock *)new_box(B, LK_BLOCK, el, s, sizeof(LBlock));
        blk->b.pseudo = AU_CSS_PSEUDO_MARKER;
        blk->b.inner = LI_REPLACED;
        blk->b.replaced = (uint8_t)shape;
        blk->b.flags = LF_ANON | (inside ? LF_INLINE_LEVEL : LF_MARKER_OUT);
        blk->rep = ar_alloc(&B->L->tree, sizeof *blk->rep);
        append(item, &blk->b);
        B->L->stats.blocks++;
        if (inside)
            B->last_space = false;
    } else if (inside) {
        LBox *box = new_box(B, LK_INLINE, el, s, sizeof(LBox));
        box->pseudo = AU_CSS_PSEUDO_MARKER;
        box->flags = ms ? 0 : LF_ANON;
        append(item, box);
        B->L->stats.inlines++;
        bool saved = B->last_space;
        B->last_space = false;
        LText *tx = (LText *)new_box(B, LK_TEXT, el, s, sizeof(LText));
        tx->text = ar_strndup(&B->L->tree, text.data, text.len);
        tx->len = (uint32_t)text.len;
        append(box, &tx->b);
        B->L->stats.texts++;
        B->last_space = saved && text.len && text.data[text.len - 1] == ' ';
    } else {
        LBlock *blk = (LBlock *)new_box(B, LK_BLOCK, el, s, sizeof(LBlock));
        blk->b.pseudo = AU_CSS_PSEUDO_MARKER;
        blk->b.flags = LF_MARKER_OUT | LF_BFC | LF_ANON | LF_INLINES;
        blk->b.inner = LI_FLOW;
        LText *tx = (LText *)new_box(B, LK_TEXT, el, s, sizeof(LText));
        /* An outside marker keeps its trailing space (white-space: pre). */
        tx->text = ar_strndup(&B->L->tree, text.data, text.len);
        tx->len = (uint32_t)text.len;
        append(&blk->b, &tx->b);
        append(item, &blk->b);
        B->L->stats.blocks++;
        B->L->stats.texts++;
    }
    au_buf_free(&text);
}

/* ---- Element boxes ---- */

/* The select is a list box: multiple, or a display size above 1. */
static bool is_listbox(const AuElement *e)
{
    if (au_element_get_attr(e, AU_ATOM_multiple))
        return true;
    const char *sz = au_element_get_attr(e, AU_ATOM_size);
    return sz && atoi(sz) > 1;
}

static bool starts_ci(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strlen(s) >= n && au_ieq(s, n, prefix, n);
}

/* <object> and <embed> (HTML, "The object element"): an image when the
 * resource is one (its type attribute, else a data: URL's type, says
 * image/..., or nothing says otherwise) and it loads or is loading. An
 * object that is not an image, or whose image failed, shows its fallback
 * content (its children); an embed then shows an empty box. Nested
 * documents and plugins are not supported, so text/html and plugin types
 * take the fallback. */
static int embedded_kind(AuLayout *L, AuElement *e, bool object, bool *skip_children)
{
    const char *url = attr_s(&e->node, object ? "data" : "src");
    const char *type = au_element_get_attr(e, AU_ATOM_type);
    int none = object ? LR_NONE : LR_OTHER;
    *skip_children = !object;
    /* The document's owner knows when the resource is a nested document,
     * or failed (src/page/frames.c). */
    int ek = L->env.embed_kind ? L->env.embed_kind(L->env.ctx, e) : 0;
    if (ek == 1) {
        *skip_children = true;
        return LR_OTHER;
    }
    if (ek == 2)
        return none;
    if (!url || !*url)
        return none;
    if (type && *type && !starts_ci(type, "image/"))
        return none;
    if ((!type || !*type) && starts_ci(url, "data:") && !starts_ci(url + 5, "image/"))
        return none;
    char abs[2048];
    resolve_url(L, url, abs, sizeof abs);
    int w = 0, h = 0;
    const AuSurface *px = NULL;
    L->images_shape = true;
    if (L->env.image && L->env.image(L->env.ctx, abs, &w, &h, &px)) {
        *skip_children = true;
        return LR_IMAGE;
    }
    return none;
}

/* The replaced or control kind of an element box. *flow is set for the
 * controls laid out as block containers (<button>, <textarea>). */
static int replaced_kind(AuLayout *L, AuNode *node, bool *skip_children, bool *flow)
{
    *flow = false;
    if (!au_is_element(node))
        return LR_NONE;
    AuElement *e = au_as_element(node);
    if (e->ns == AU_NS_SVG && e->tag == AU_ATOM_svg) {
        *skip_children = true;
        return LR_SVG;
    }
    if (e->ns != AU_NS_HTML)
        return LR_NONE;
    AuAtom t = e->tag;
    if (t == AU_ATOM_img)
        return LR_IMAGE;
    if (t == AU_ATOM_input) {
        const char *ty = au_element_get_attr(e, AU_ATOM_type);
        if (!ty)
            return LR_TEXT_INPUT;
        size_t n = strlen(ty);
        if (au_ieq(ty, n, "checkbox", 8))
            return LR_CHECKBOX;
        if (au_ieq(ty, n, "radio", 5))
            return LR_RADIO;
        if (au_ieq(ty, n, "submit", 6) || au_ieq(ty, n, "button", 6) || au_ieq(ty, n, "reset", 5))
            return LR_BUTTON;
        if (au_ieq(ty, n, "file", 4))
            return LR_FILE;
        if (au_ieq(ty, n, "color", 5))
            return LR_COLOR;
        if (au_ieq(ty, n, "image", 5))
            return LR_IMAGE;
        if (au_ieq(ty, n, "range", 5))
            return LR_RANGE;
        return LR_TEXT_INPUT; /* text, search, email, url, tel, password, number, date and time types */
    }
    if (t == AU_ATOM_select) {
        *skip_children = true;
        return is_listbox(e) ? LR_LISTBOX : LR_SELECT;
    }
    if (t == AU_ATOM_textarea) {
        /* Its value is painted by controls.c, not laid out as children. */
        *skip_children = true;
        return LR_TEXTAREA;
    }
    if (t == AU_ATOM_button) {
        *flow = true;
        return LR_BUTTON;
    }
    if (t == AU_ATOM_progress || t == AU_ATOM_meter) {
        *skip_children = true;
        return t == AU_ATOM_progress ? LR_PROGRESS : LR_METER;
    }
    if (t == AU_ATOM_object || t == AU_ATOM_embed)
        return embedded_kind(L, e, t == AU_ATOM_object, skip_children);
    if (t == AU_ATOM_canvas) {
        /* With script off a canvas shows its children (the fallback). */
        const AuSurface *px = NULL;
        if (L->env.canvas && L->env.canvas(L->env.ctx, e, &px) < 0)
            return LR_NONE;
        *skip_children = true;
        return LR_CANVAS;
    }
    if (t == AU_ATOM_video || t == AU_ATOM_iframe || t == AU_ATOM_audio || t == AU_ATOM_frame) {
        *skip_children = true;
        return LR_OTHER;
    }
    return LR_NONE;
}

/* The fieldset's first legend child is its rendered legend. */
static bool is_rendered_legend(AuNode *legend)
{
    AuNode *p = legend->parent;
    if (!p || !au_is_html_element(p, AU_ATOM_fieldset))
        return false;
    for (AuNode *c = p->first_child; c && c != legend; c = c->next)
        if (au_is_html_element(c, AU_ATOM_legend))
            return false;
    return true;
}

static void text_of(AuNode *n, AuBuf *out, int limit)
{
    for (AuNode *c = au_node_next(n, n); c && (int)out->len < limit; c = au_node_next(c, n))
        if (c->type == AU_NODE_TEXT)
            au_buf_append(out, au_as_text(c)->data, au_as_text(c)->len);
}

/* Appends the text of option (or an optgroup's label) to out with white
 * space collapsed and trimmed (HTML: the option's label). */
static void option_label(AuNode *opt, AuBuf *out)
{
    const char *lab = attr_s(opt, "label");
    AuBuf t;
    au_buf_init(&t);
    if (lab && (*lab || au_is_html_element(opt, AU_ATOM_optgroup)))
        au_buf_puts(&t, lab);
    else if (au_is_html_element(opt, AU_ATOM_option))
        text_of(opt, &t, 200);
    bool sp = true;
    size_t start = out->len;
    for (size_t i = 0; i < t.len; i++) {
        char c = t.data[i];
        if (au_is_space((unsigned char)c)) {
            if (!sp)
                au_buf_putc(out, ' ');
            sp = true;
        } else {
            au_buf_putc(out, c);
            sp = false;
        }
    }
    while (out->len > start && out->data[out->len - 1] == ' ')
        out->len--;
    au_buf_free(&t);
}

static bool is_option_selected(AuNode *o)
{
    AuElement *e = au_as_element(o);
    if (e->state & AU_STATE_CHECKED_SET)
        return (e->state & AU_STATE_CHECKED) != 0;
    return au_element_get_attr(e, AU_ATOM_selected) != NULL;
}

/* The options and option groups of a select, in order: its option and
 * optgroup children and the options of those groups (HTML "list of
 * options"; options nested deeper are not listed). */
static AuNode *next_option(AuNode *sel, AuNode *cur)
{
    AuNode *c;
    if (!cur)
        c = sel->first_child;
    else if (au_is_html_element(cur, AU_ATOM_optgroup) && cur->parent == sel && cur->first_child)
        c = cur->first_child;
    else
        c = cur->next;
    for (;;) {
        while (!c && cur && cur->parent != sel) {
            cur = cur->parent;
            c = cur->next;
        }
        if (!c)
            return NULL;
        if (au_is_html_element(c, AU_ATOM_option) || (au_is_html_element(c, AU_ATOM_optgroup) && c->parent == sel))
            return c;
        cur = c;
        c = c->next;
    }
}

static void setup_select(Builder *B, LBlock *k, LReplaced *rep, AuBuf *t)
{
    AuNode *n = k->b.node;
    if (k->b.replaced == LR_SELECT) {
        AuNode *chosen = NULL, *first = NULL;
        for (AuNode *o = next_option(n, NULL); o; o = next_option(n, o)) {
            if (!au_is_html_element(o, AU_ATOM_option))
                continue;
            if (!first)
                first = o;
            if (is_option_selected(o))
                chosen = o; /* the last selected one wins in a single select */
        }
        if (!chosen)
            chosen = first;
        if (chosen)
            option_label(chosen, t);
        rep->label = ar_strndup(&B->L->tree, t->data ? t->data : "", t->len);
        /* The widest option sets the width: kept as the second string. */
        AuBuf w;
        au_buf_init(&w);
        float best = -1;
        LFont *f = l_font(B->L, k->b.style);
        for (AuNode *o = next_option(n, NULL); o; o = next_option(n, o)) {
            if (!au_is_html_element(o, AU_ATOM_option))
                continue;
            size_t at = w.len;
            option_label(o, &w);
            float ow = l_measure(B->L, f, k->b.style, w.data + at, w.len - at);
            if (ow > best) {
                best = ow;
                if (w.len > at) /* an empty label leaves w.data NULL */
                    memmove(w.data, w.data + at, w.len - at);
                w.len -= at;
            } else {
                w.len = at;
            }
        }
        rep->aux = ar_strndup(&B->L->tree, w.data ? w.data : "", w.len);
        au_buf_free(&w);
        return;
    }
    /* List box: one row per option and group label. */
    uint32_t nrows = 0;
    for (AuNode *o = next_option(n, NULL); o && nrows < 10000; o = next_option(n, o)) {
        uint8_t fl = 0;
        if (au_is_html_element(o, AU_ATOM_optgroup))
            fl |= LB_GROUP;
        else if (is_option_selected(o))
            fl |= LB_SELECTED;
        if (o->parent != n)
            fl |= LB_INDENT;
        if (attr_s(o, "disabled") || (o->parent != n && attr_s(o->parent, "disabled")))
            fl |= LB_DISABLED;
        if (nrows)
            au_buf_putc(t, '\n');
        au_buf_putc(t, (char)(0x40 | fl));
        size_t at = t->len;
        option_label(o, t);
        for (size_t i = at; i < t->len; i++)
            if (t->data[i] == '\n')
                t->data[i] = ' ';
        nrows++;
    }
    rep->label = ar_strndup(&B->L->tree, t->data ? t->data : "", t->len);
    const char *sz = attr_s(n, "size");
    int rows = sz ? atoi(sz) : 0;
    if (rows <= 1)
        rows = 4; /* multiple without a size */
    rep->rows = (uint16_t)(rows > 1000 ? 1000 : rows);
}

static void setup_replaced(Builder *B, LBlock *k)
{
    LReplaced *rep = ar_alloc(&B->L->tree, sizeof *rep);
    k->rep = rep;
    AuNode *n = k->b.node;
    AuBuf t;
    au_buf_init(&t);
    switch (k->b.replaced) {
    case LR_IMAGE: {
        if (au_is_html_element(n, AU_ATOM_img)) {
            /* srcset, sizes and <picture> choose the URL. */
            size_t ulen = 0;
            bool viewport = false;
            AuLayout *L = B->L;
            float density;
            const char *u = img_source(n, L->src_vw > 0 ? L->src_vw : 1024, L->src_vh > 0 ? L->src_vh : 768, &ulen,
                                       &viewport, &density);
            /* The natural size is divided by the candidate's density. */
            rep->density = density;
            if (viewport)
                L->src_viewport = true;
            if (u && ulen) {
                au_buf_clear(&t);
                au_buf_append(&t, u, ulen);
                image_lookup(L, rep, au_buf_cstr(&t));
                au_buf_clear(&t);
            }
        } else {
            image_lookup(B->L, rep, attr_s(n, au_is_html_element(n, AU_ATOM_object) ? "data" : "src"));
        }
        const char *alt = attr_s(n, "alt");
        if (!alt && au_is_html_element(n, AU_ATOM_input))
            alt = "Submit"; /* image button without alt text */
        if (alt)
            rep->alt = ar_strndup(&B->L->tree, alt, strlen(alt));
        if (!rep->loaded)
            rep->label = rep->alt;
        break;
    }
    case LR_BUTTON: {
        if (k->b.inner != LI_REPLACED)
            break; /* <button>: its content */
        const char *v = attr_s(n, "value");
        const char *ty = attr_s(n, "type");
        if (!v)
            v = ty && au_ieq(ty, strlen(ty), "reset", 5)    ? "Reset"
                : ty && au_ieq(ty, strlen(ty), "button", 6) ? ""
                                                            : "Submit";
        rep->label = ar_strndup(&B->L->tree, v, strlen(v));
        break;
    }
    case LR_FILE:
        rep->label = "Choose File";
        rep->aux = attr_s(n, "multiple") ? "No files chosen" : "No file chosen";
        break;
    case LR_TEXT_INPUT: {
        const char *v = attr_s(n, "value");
        const char *ty = attr_s(n, "type");
        if (!v || !*v) {
            v = attr_s(n, "placeholder");
            rep->placeholder = true;
        } else if (ty && au_ieq(ty, strlen(ty), "password", 8)) {
            size_t len = 0;
            for (const char *p = v; *p; p++)
                len += ((unsigned char)*p & 0xC0) != 0x80; /* code points */
            for (size_t i = 0; i < len && i < 256; i++)
                au_buf_puts(&t, "\xE2\x80\xA2");
            v = au_buf_cstr(&t);
        }
        if (v) {
            /* A value has no line breaks (HTML value sanitization). */
            size_t len = strlen(v);
            char *s = ar_strndup(&B->L->tree, v, len);
            for (size_t i = 0; i < len; i++)
                if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
                    s[i] = ' ';
            rep->label = s;
        }
        break;
    }
    case LR_SELECT:
    case LR_LISTBOX:
        setup_select(B, k, rep, &t);
        break;
    case LR_TEXTAREA: {
        const char *cs = attr_s(n, "cols"), *rs = attr_s(n, "rows");
        int cols = cs ? atoi(cs) : 0, rows = rs ? atoi(rs) : 0;
        rep->cols = (uint16_t)(cols > 0 && cols <= 1000 ? cols : 20);
        rep->rows = (uint16_t)(rows > 0 && rows <= 1000 ? rows : 2);
        bool empty = true;
        for (AuNode *c = n->first_child; c && empty; c = c->next)
            if (c->type == AU_NODE_TEXT && au_as_text(c)->len)
                empty = false;
        const char *ph = attr_s(n, "placeholder");
        if (empty && ph && *ph)
            rep->aux = ar_strndup(&B->L->tree, ph, strlen(ph));
        break;
    }
    default:
        break;
    }
    au_buf_free(&t);
}

static LBox *make_element_box(Builder *B, AuNode *node, const AuStyle *s, int pseudo, bool *skip_children)
{
    const AuStyleBox *bx = s->box;
    uint16_t d = bx->display;
    int outer = au_display_outer(d), inner = au_display_inner(d);
    *skip_children = false;
    bool flow_control = false;
    int rk = pseudo ? LR_NONE : replaced_kind(B->L, node, skip_children, &flow_control);
    LBox *b;
    if (!pseudo && au_is_html_element(node, AU_ATOM_br)) {
        b = new_box(B, LK_BR, node, s, sizeof(LBox));
        *skip_children = true;
        return b;
    }
    if (!pseudo && au_is_html_element(node, AU_ATOM_wbr)) {
        b = new_box(B, LK_WBR, node, s, sizeof(LBox));
        *skip_children = true;
        return b;
    }
    bool abspos = bx->position == AU_POSITION_ABSOLUTE || bx->position == AU_POSITION_FIXED;
    bool floated = !abspos && bx->float_ != AU_FLOAT_NONE;
    if (rk == LR_NONE && outer == AU_DISPLAY_OUTER_INLINE && inner == AU_DISPLAY_INNER_FLOW && !abspos &&
        !floated) {
        b = new_box(B, LK_INLINE, node, s, sizeof(LBox));
        B->L->stats.inlines++;
    } else if (rk == LR_NONE && (inner == AU_DISPLAY_INNER_RUBY || inner == AU_DISPLAY_INNER_RUBY_BASE ||
                                 inner == AU_DISPLAY_INNER_RUBY_TEXT ||
                                 inner == AU_DISPLAY_INNER_RUBY_BASE_CONTAINER ||
                                 inner == AU_DISPLAY_INNER_RUBY_TEXT_CONTAINER) &&
               !abspos && !floated) {
        /* Ruby is laid out inline. */
        b = new_box(B, LK_INLINE, node, s, sizeof(LBox));
        B->L->stats.inlines++;
    } else {
        size_t size = sizeof(LBlock);
        if (inner == AU_DISPLAY_INNER_TABLE)
            size = sizeof(LTableBox);
        else if (inner == AU_DISPLAY_INNER_TABLE_CELL)
            size = sizeof(LCellBox);
        LBlock *k = (LBlock *)new_box(B, LK_BLOCK, node, s, size);
        b = &k->b;
        B->L->stats.blocks++;
        if (rk != LR_NONE && !flow_control) {
            b->inner = LI_REPLACED;
            b->replaced = (uint8_t)rk;
        } else {
            /* A <button> keeps its display's inner type (flow, flex, grid). */
            if (rk == LR_BUTTON) {
                b->replaced = (uint8_t)rk;
                b->flags |= LF_BFC;
            }
            switch (inner) {
            case AU_DISPLAY_INNER_FLOW_ROOT: b->inner = LI_FLOW; b->flags |= LF_BFC; break;
            case AU_DISPLAY_INNER_FLEX: b->inner = LI_FLEX; b->flags |= LF_BFC; break;
            case AU_DISPLAY_INNER_GRID: b->inner = LI_GRID; b->flags |= LF_BFC; break;
            case AU_DISPLAY_INNER_TABLE: b->inner = LI_TABLE; b->flags |= LF_BFC; break;
            case AU_DISPLAY_INNER_TABLE_ROW_GROUP:
            case AU_DISPLAY_INNER_TABLE_HEADER_GROUP:
            case AU_DISPLAY_INNER_TABLE_FOOTER_GROUP: b->inner = LI_TABLE_ROW_GROUP; break;
            case AU_DISPLAY_INNER_TABLE_ROW: b->inner = LI_TABLE_ROW; break;
            case AU_DISPLAY_INNER_TABLE_CELL: b->inner = LI_TABLE_CELL; b->flags |= LF_BFC; break;
            case AU_DISPLAY_INNER_TABLE_CAPTION: b->inner = LI_FLOW; b->flags |= LF_BFC; break;
            case AU_DISPLAY_INNER_TABLE_COLUMN:
                b->inner = LI_TABLE_COLUMN;
                *skip_children = true;
                break;
            case AU_DISPLAY_INNER_TABLE_COLUMN_GROUP: b->inner = LI_TABLE_COLUMN_GROUP; break;
            case AU_DISPLAY_INNER_WEBKIT_BOX:
                b->inner = LI_FLOW;
                b->flags |= LF_BFC;
                if (s->rare->line_clamp != AU_INT_NONE && s->rare->line_clamp > 0)
                    b->flags |= LF_LINE_CLAMP;
                break;
            default: b->inner = LI_FLOW; break;
            }
        }
        if (outer == AU_DISPLAY_OUTER_INLINE && !abspos && !floated)
            b->flags |= LF_INLINE_LEVEL | LF_BFC;
        if (floated)
            b->flags |= LF_FLOAT | LF_BFC;
        if (abspos)
            b->flags |= LF_ABSPOS | LF_BFC | (bx->position == AU_POSITION_FIXED ? LF_FIXED : 0);
        if (bx->overflow_x != AU_OVERFLOW_VISIBLE || bx->overflow_y != AU_OVERFLOW_VISIBLE) {
            /* overflow: clip does not establish a BFC; the others do. */
            if (bx->overflow_x != AU_OVERFLOW_CLIP || bx->overflow_y != AU_OVERFLOW_CLIP)
                b->flags |= LF_BFC;
            /* Overflow does not apply to tables, rows and row groups (as
             * in browsers); cells clip. */
            bool tabular = b->inner == LI_TABLE || b->inner == LI_TABLE_ROW || b->inner == LI_TABLE_ROW_GROUP ||
                           b->inner == LI_TABLE_COLUMN || b->inner == LI_TABLE_COLUMN_GROUP;
            if (tabular && b->inner != LI_TABLE)
                b->flags &= ~LF_BFC;
            if (b->inner != LI_REPLACED && !tabular) {
                b->flags |= LF_CLIPS;
                if (bx->overflow_x == AU_OVERFLOW_SCROLL || bx->overflow_x == AU_OVERFLOW_AUTO ||
                    bx->overflow_y == AU_OVERFLOW_SCROLL || bx->overflow_y == AU_OVERFLOW_AUTO)
                    b->flags |= LF_SCROLLER;
            }
        }
        if (s->rare->contain & (AU_CONTAIN_LAYOUT | AU_CONTAIN_PAINT))
            b->flags |= LF_BFC;
        /* A fieldset establishes a block formatting context (HTML,
         * "The fieldset and legend elements"). */
        if (!pseudo && au_is_html_element(node, AU_ATOM_fieldset))
            b->flags |= LF_BFC;
        if (!pseudo && au_is_html_element(node, AU_ATOM_legend) && !floated && !abspos && is_rendered_legend(node)) {
            b->flags |= LF_LEGEND;
            b->flags &= ~LF_INLINE_LEVEL; /* the rendered legend is blockified */
            b->flags |= LF_BFC;
        }
        if (rk != LR_NONE)
            setup_replaced(B, k);
    }
    b->pseudo = (uint8_t)pseudo;
    if (bx->position == AU_POSITION_RELATIVE)
        b->flags |= LF_RELPOS;
    else if (bx->position == AU_POSITION_STICKY)
        b->flags |= LF_STICKY;
    /* A box with a transform, perspective, a filter or layout or paint
     * containment is the containing block of its absolutely positioned
     * and fixed descendants (CSS Transforms 1 section 3, Transforms 2,
     * Filter Effects 1 section 5, Containment 1 sections 3.3 and 3.4,
     * will-change for those properties). Inline boxes are not
     * transformable. */
    if (b->kind == LK_BLOCK && l_style_contains_fixed(s))
        b->flags |= LF_TRANSFORM;
    if (s->inh->visibility != AU_VISIBILITY_VISIBLE)
        b->flags |= LF_HIDDEN;
    if (au_display_list_item(d))
        b->flags |= LF_LIST_ITEM;
    return b;
}

/* ---- Normalization ---- */

static bool collapsible_ws_text(const LBox *b)
{
    if (b->kind != LK_TEXT)
        return false;
    if (b->style->inh->white_space_collapse != AU_WHITE_SPACE_COLLAPSE_COLLAPSE &&
        b->style->inh->white_space_collapse != AU_WHITE_SPACE_COLLAPSE_PRESERVE_BREAKS)
        return false;
    const LText *t = (const LText *)b;
    for (uint32_t i = 0; i < t->len; i++)
        if (t->text[i] != ' ' && t->text[i] != '\n')
            return false;
    return true;
}

static LBox *new_anon_block(Builder *B, LBox *parent)
{
    LBlock *k = (LBlock *)new_box(B, LK_BLOCK, NULL, parent->style, sizeof(LBlock));
    k->b.flags = LF_ANON | LF_INLINES;
    k->b.inner = LI_FLOW;
    if (parent->flags & LF_HIDDEN)
        k->b.flags |= LF_HIDDEN;
    B->L->stats.blocks++;
    return &k->b;
}

/* Rebuilds parent's child list, wrapping runs of inline-level children.
 * flex: every run of non-block children with real content becomes an
 * anonymous flex item. */
static void wrap_inline_runs(Builder *B, LBox *parent, bool flex)
{
    LBox *c = parent->first;
    parent->first = parent->last = NULL;
    while (c) {
        LBox *next = c->next;
        bool inl = !is_block_level(c) && !is_out_of_flow(c);
        if (flex && c->kind == LK_BLOCK && (c->flags & LF_INLINE_LEVEL))
            inl = false; /* atomic inlines are blockified items already */
        if (!inl) {
            append(parent, c);
            c = next;
            continue;
        }
        /* Collect the run [c, end). */
        LBox *end = c;
        bool content = false;
        while (end) {
            bool i2 = !is_block_level(end) && !is_out_of_flow(end);
            if (flex && end->kind == LK_BLOCK && (end->flags & LF_INLINE_LEVEL))
                i2 = false;
            if (!i2 && !(is_out_of_flow(end) && !flex))
                break;
            if (i2 && !collapsible_ws_text(end))
                content = true;
            end = end->next;
        }
        if (!content) {
            /* Only white space (and out-of-flow boxes): drop the text. */
            while (c != end) {
                LBox *nx = c->next;
                if (c->kind != LK_TEXT)
                    append(parent, c);
                c = nx;
            }
            continue;
        }
        LBox *anon = new_anon_block(B, parent);
        if (flex)
            anon->flags |= LF_FLEX_ITEM | LF_BFC;
        append(parent, anon);
        while (c != end) {
            LBox *nx = c->next;
            append(anon, c);
            c = nx;
        }
    }
}

/* ---- Table fixup (CSS 2.1 17.2.1, CSS Tables 3 "Fixup") ----
 *
 * Children of a table that are not captions, columns, column groups, row
 * groups or rows are wrapped in anonymous rows; children of a row group
 * that are not rows likewise; children of a row that are not cells in
 * anonymous cells. A run of table-internal boxes anywhere else is wrapped
 * in an anonymous table (inline-level inside an inline box), whose own
 * fixup then adds the rows. White space between table parts is dropped. */

static bool is_tabular(const LBox *b)
{
    return b->kind == LK_BLOCK && (b->inner == LI_TABLE || b->inner == LI_TABLE_ROW_GROUP || b->inner == LI_TABLE_ROW);
}

/* Is c a proper child of tabular container p? */
static bool proper_child(const LBox *p, const LBox *c)
{
    if (c->kind != LK_BLOCK)
        return false;
    switch (p->inner) {
    case LI_TABLE:
        return c->inner == LI_TABLE_ROW_GROUP || c->inner == LI_TABLE_ROW || c->inner == LI_TABLE_COLUMN ||
               c->inner == LI_TABLE_COLUMN_GROUP || l_is_caption(c);
    case LI_TABLE_ROW_GROUP: return c->inner == LI_TABLE_ROW;
    default: return c->inner == LI_TABLE_CELL;
    }
}

static LBox *new_anon_table_part(Builder *B, LBox *parent, int inner)
{
    size_t size = inner == LI_TABLE ? sizeof(LTableBox) : inner == LI_TABLE_CELL ? sizeof(LCellBox) : sizeof(LBlock);
    LBlock *k = (LBlock *)new_box(B, LK_BLOCK, NULL, parent->style, size);
    k->b.flags = LF_ANON;
    k->b.inner = (uint8_t)inner;
    if (inner == LI_TABLE || inner == LI_TABLE_CELL)
        k->b.flags |= LF_BFC;
    if (parent->flags & LF_HIDDEN)
        k->b.flags |= LF_HIDDEN;
    B->L->stats.blocks++;
    return &k->b;
}

/* Releases a box removed from the tree, and its subtree (the memory stays
 * in the arena). */
static void drop_box(LBox *root)
{
    root->next = NULL;
    root->parent = NULL;
    LBox *b = root;
    while (b) {
        LBox *next;
        if (b->first) {
            next = b->first;
            b->first = NULL;
        } else {
            if (b->node && au_is_element(b->node) && au_as_element(b->node)->box == b)
                au_as_element(b->node)->box = NULL;
            if (b->node)
                au_node_unref(b->node);
            b->node = NULL;
            au_style_release(b->style);
            b->style = NULL;
            next = b == root ? NULL : (b->next ? b->next : b->parent);
        }
        b = next;
    }
}

static void fix_table_children(Builder *B, LBox *p)
{
    LBox *c = p->first;
    p->first = p->last = NULL;
    if (p->kind == LK_BLOCK && p->inner == LI_TABLE_COLUMN) {
        while (c) { /* a column has no content */
            LBox *nx = c->next;
            drop_box(c);
            c = nx;
        }
        return;
    }
    if (p->kind == LK_BLOCK && p->inner == LI_TABLE_COLUMN_GROUP) {
        /* Only columns stay; anything else in a column group is dropped. */
        while (c) {
            LBox *nx = c->next;
            if (c->kind == LK_BLOCK && c->inner == LI_TABLE_COLUMN)
                append(p, c);
            else
                drop_box(c);
            c = nx;
        }
        return;
    }
    bool tabular = is_tabular(p);
    while (c) {
        LBox *next = c->next;
        if (tabular) {
            if (proper_child(p, c) || (c->kind == LK_BLOCK && (c->flags & LF_ABSPOS))) {
                append(p, c);
                c = next;
                continue;
            }
            /* The run of children that are not proper. */
            LBox *end = c;
            bool content = false;
            while (end && !proper_child(p, end)) {
                if (!collapsible_ws_text(end) && !(end->kind == LK_BLOCK && (end->flags & LF_ABSPOS)))
                    content = true;
                end = end->next;
            }
            if (!content) {
                while (c != end) {
                    LBox *nx = c->next;
                    if (c->kind != LK_TEXT)
                        append(p, c);
                    else
                        drop_box(c);
                    c = nx;
                }
                continue;
            }
            int inner = p->inner == LI_TABLE_ROW ? LI_TABLE_CELL : LI_TABLE_ROW;
            LBox *anon = new_anon_table_part(B, p, inner);
            append(p, anon);
            while (c != end) {
                LBox *nx = c->next;
                append(anon, c);
                c = nx;
            }
            continue;
        }
        if (!is_table_internal(c)) {
            append(p, c);
            c = next;
            continue;
        }
        /* A run of table-internal boxes, with the white space between them. */
        LBox *last = c, *end = c->next;
        while (end && (is_table_internal(end) || collapsible_ws_text(end))) {
            if (is_table_internal(end))
                last = end;
            end = end->next;
        }
        end = last->next;
        LBox *anon = new_anon_table_part(B, p, LI_TABLE);
        if (p->kind == LK_INLINE)
            anon->flags |= LF_INLINE_LEVEL;
        append(p, anon);
        while (c != end) {
            LBox *nx = c->next;
            append(anon, c);
            c = nx;
        }
    }
}

static void table_fixup(Builder *B)
{
    LBox *b = B->L->root;
    while (b) {
        if ((b->kind == LK_BLOCK && b->inner != LI_REPLACED) || b->kind == LK_INLINE) {
            /* Columns and column groups always: their content (generated
             * content included) is dropped. */
            bool any = is_tabular(b) || (b->kind == LK_BLOCK && (b->inner == LI_TABLE_COLUMN_GROUP ||
                                                                 b->inner == LI_TABLE_COLUMN));
            for (LBox *c = b->first; c && !any; c = c->next)
                any = is_table_internal(c);
            if (any)
                fix_table_children(B, b);
        }
        if (b->first && b->kind != LK_TEXT)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
}

static void normalize(Builder *B)
{
    /* Postorder is not needed: each container's fix-up only looks at its
     * own children. Preorder walk with an explicit next-pointer loop. */
    LBox *b = B->L->root;
    while (b) {
        if (b->kind == LK_BLOCK && (is_tabular(b) || b->inner == LI_TABLE_COLUMN ||
                                    b->inner == LI_TABLE_COLUMN_GROUP)) {
            /* Children are table parts after the fixup. */
        } else if (b->kind == LK_BLOCK && b->inner != LI_REPLACED) {
            bool any_block = false, any_inline = false;
            for (LBox *c = b->first; c; c = c->next) {
                if (is_block_level(c))
                    any_block = true;
                else if (!is_out_of_flow(c))
                    any_inline = true;
            }
            if (b->inner == LI_FLEX || b->inner == LI_GRID) {
                wrap_inline_runs(B, b, true);
                for (LBox *c = b->first; c; c = c->next)
                    if (c->kind == LK_BLOCK && !(c->flags & LF_ABSPOS)) {
                        c->flags |= LF_FLEX_ITEM | LF_BFC;
                        c->flags &= ~(LF_FLOAT | LF_INLINE_LEVEL);
                    }
            } else if (b->inner == LI_TABLE || b->inner == LI_TABLE_ROW_GROUP || b->inner == LI_TABLE_ROW) {
                /* Placeholder tables: stray text is wrapped; white space dropped. */
                wrap_inline_runs(B, b, false);
                for (LBox *c = b->first; c; c = c->next)
                    c->flags &= ~LF_INLINE_LEVEL;
            } else if (any_block) {
                /* Wrap inline runs; drop white space between blocks. */
                (void)any_inline;
                wrap_inline_runs(B, b, false);
            } else {
                b->flags |= LF_INLINES;
            }
        }
        /* next in preorder */
        if (b->first && b->kind != LK_TEXT)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
}

/* ---- The walk ---- */

static void push(Builder *B, AuNode *dom, LBox *box, const AuStyle *s)
{
    if (B->n == B->cap) {
        B->cap = B->cap ? B->cap * 2 : 64;
        B->st = au_realloc(B->st, B->cap * sizeof *B->st);
    }
    BFrame *f = &B->st[B->n++];
    memset(f, 0, sizeof *f);
    f->dom = dom;
    f->child = dom ? dom->first_child : NULL;
    f->box = box;
    f->style = s;
}

static void pop(Builder *B)
{
    B->n--;
    /* Counters created by this frame's children end with it. */
    while (B->nctr && B->ctr[B->nctr - 1].owner > B->n)
        B->nctr--;
}

void l_build_tree(AuLayout *L)
{
    l_free_tree(L);
    L->images_shape = false;
    L->fixed_backgrounds = false;
    L->src_viewport = false;
    AuElement *rootel = L->doc ? au_document_element(L->doc) : NULL;
    if (!rootel || !rootel->style)
        return;
    const AuStyle *rs = au_element_style(rootel);
    if (rs->box->display == AU_DISPLAY_NONE)
        return;
    Builder B;
    memset(&B, 0, sizeof B);
    B.L = L;
    au_buf_init(&B.tmp);
    B.last_space = true;
    /* The root box: always a block (CSS blockifies the root). */
    bool skip;
    LBox *root = make_element_box(&B, &rootel->node, rs, 0, &skip);
    if (root->kind != LK_BLOCK) {
        /* display: contents or inline on the root: wrap in an anonymous block. */
        LBox *anon = new_box(&B, LK_BLOCK, &rootel->node, rs, sizeof(LBlock));
        anon->flags = LF_ANON;
        append(anon, root);
        root = anon;
    }
    root->flags |= LF_BFC;
    root->flags &= ~(LF_FLOAT | LF_ABSPOS | LF_FIXED | LF_INLINE_LEVEL);
    L->root = root;
    rootel->box = root;
    apply_counters(&B, rs, &rootel->node, false, false);
    push(&B, &rootel->node, root, rs);
    if (skip)
        B.st[0].skip_children = true;
    while (B.n) {
        BFrame *f = &B.st[B.n - 1];
        if (f->stage == 0) {
            f->stage = 1;
            if (f->box && f->style && f->dom && f->box->node == f->dom && f->box->pseudo == 0) {
                if ((f->box->flags & LF_LIST_ITEM) && f->box->kind == LK_BLOCK)
                    make_marker(&B, f->box, f->dom, f->style);
                if (f->box->inner != LI_REPLACED && f->box->replaced != LR_TEXTAREA)
                    make_pseudo(&B, f->box, f->dom, f->style, AU_CSS_PSEUDO_BEFORE);
            }
            if (f->skip_children)
                f->child = NULL;
            continue;
        }
        if (f->stage == 1) {
            AuNode *c = f->child;
            if (!c) {
                f->stage = 2;
                LBox *fb = f->box;
                if (fb && f->style && f->dom && f->dom->type == AU_NODE_ELEMENT &&
                    !(fb->node == f->dom && fb->kind == LK_BLOCK &&
                      (fb->inner == LI_REPLACED || fb->replaced == LR_TEXTAREA)))
                    make_pseudo(&B, fb, f->dom, f->style, AU_CSS_PSEUDO_AFTER);
                if (f->out_of_flow)
                    B.last_space = f->saved_space;
                else if (fb && fb->kind == LK_BLOCK && fb->node == f->dom && !(fb->flags & LF_INLINE_LEVEL))
                    B.last_space = true;
                else if (fb && fb->kind == LK_BLOCK && fb->node == f->dom)
                    B.last_space = false;
                pop(&B);
                continue;
            }
            f->child = c->next;
            if (f->details_closed) {
                /* A closed details shows its first summary child only. */
                if (!au_is_html_element(c, AU_ATOM_summary) || f->summary_done)
                    continue;
                f->summary_done = true;
            }
            if (c->type == AU_NODE_TEXT) {
                add_text(&B, f->box, c, f->style, au_as_text(c)->data, au_as_text(c)->len);
                continue;
            }
            if (c->type != AU_NODE_ELEMENT)
                continue;
            AuElement *e = au_as_element(c);
            const AuStyle *s = au_element_style(e);
            if (!s || s->box->display == AU_DISPLAY_NONE) {
                /* Remembered, so a restyle that shows it rebuilds the tree
                 * (au_layout_styles_changed sees only elements with boxes). */
                if (s && !f->skip_children) {
                    if (L->nhidden == L->hidden_cap) {
                        L->hidden_cap = L->hidden_cap ? L->hidden_cap * 2 : 64;
                        L->hidden = au_realloc(L->hidden, L->hidden_cap * sizeof *L->hidden);
                    }
                    L->hidden[L->nhidden++] = e;
                }
                continue;
            }
            bool li = au_display_list_item(s->box->display);
            bool rev = false;
            if (li && c->parent && au_is_html_element(c->parent, AU_ATOM_ol) && attr_s(c->parent, "reversed"))
                rev = true;
            apply_counters(&B, s, c, li, rev);
            if (s->box->display == AU_DISPLAY_CONTENTS) {
                push(&B, c, f->box, s);
                /* ::before/::after of a contents element go into the parent box. */
                BFrame *nf = &B.st[B.n - 1];
                nf->stage = 1;
                make_pseudo(&B, nf->box, c, s, AU_CSS_PSEUDO_BEFORE);
                continue;
            }
            LBox *box = make_element_box(&B, c, s, 0, &skip);
            LBox *parent = f->box;
            bool saved = B.last_space;
            insert_child(&B, parent, box);
            if ((box->flags & LF_LEGEND) &&
                !(box->parent && box->parent->node == c->parent && box->parent->kind == LK_BLOCK &&
                  box->parent->inner == LI_FLOW))
                box->flags &= ~LF_LEGEND; /* its fieldset has no block box of its own */
            if (!e->box)
                e->box = box;
            if (box->kind == LK_BLOCK && !(box->flags & LF_INLINE_LEVEL))
                B.last_space = true;
            if (box->kind == LK_BR || box->kind == LK_WBR) {
                if (box->kind == LK_BR && s->inh->white_space_collapse != AU_WHITE_SPACE_COLLAPSE_PRESERVE)
                    B.last_space = true;
                continue;
            }
            if (box->kind == LK_BLOCK && box->inner == LI_REPLACED) {
                B.last_space = false;
                if (skip)
                    continue;
            }
            push(&B, c, box, s);
            if (skip)
                B.st[B.n - 1].skip_children = true;
            if (au_is_html_element(c, AU_ATOM_details) && !attr_s(c, "open"))
                B.st[B.n - 1].details_closed = true;
            if (box->flags & (LF_FLOAT | LF_ABSPOS)) {
                B.st[B.n - 1].out_of_flow = true;
                B.st[B.n - 1].saved_space = saved;
            }
        }
    }
    au_free(B.st);
    au_free(B.ctr);
    au_buf_free(&B.tmp);
    table_fixup(&B);
    normalize(&B);
    for (LBox *b = L->root; b;) {
        if (b->kind == LK_BLOCK && b->inner == LI_TABLE)
            l_table_build_grid(L, (LTableBox *)b);
        if (b->first && b->kind != LK_TEXT)
            b = b->first;
        else {
            while (b && !b->next)
                b = b->parent;
            if (b)
                b = b->next;
        }
    }
    L->stats.tree_bytes = L->tree.total;
}

/* ---- Destruction ---- */

void l_box_free_layout(LBox *b)
{
    if (b->kind != LK_BLOCK)
        return;
    LBlock *k = (LBlock *)b;
    au_free(k->lines);
    au_free(k->frags);
    k->lines = NULL;
    k->frags = NULL;
    k->nlines = k->nfrags = 0;
    if (b->inner == LI_TABLE)
        l_table_free((LTableBox *)b);
    if (k->layers) {
        au_free(k->layers->items);
        au_free(k->layers);
        k->layers = NULL;
    }
}

void l_free_tree(AuLayout *L)
{
    au_svg_id_index_free(L->svg_ids);
    L->svg_ids = NULL;
    LBox *b = L->root;
    while (b) {
        LBox *next;
        if (b->first) {
            next = b->first;
            b->first = NULL; /* visit children, then come back without descending */
        } else {
            /* release this box */
            l_box_free_layout(b);
            if (b->node && au_is_element(b->node) && au_as_element(b->node)->box == b)
                au_as_element(b->node)->box = NULL;
            if (b->node)
                au_node_unref(b->node);
            b->node = NULL;
            au_style_release(b->style);
            b->style = NULL;
            next = b->next ? b->next : b->parent;
        }
        b = next;
    }
    ar_free_all(&L->tree);
    L->root = NULL;
    L->nhidden = 0;
    memset(&L->stats, 0, sizeof L->stats);
}
