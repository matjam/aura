/* Style sheet parsing per CSS Syntax Level 3 (rules, at-rules, blocks,
 * declarations, error recovery) and CSS Nesting.
 *
 * The parser walks the flat token array of css_tokenize. A component value
 * is skipped in one step through the block end index of opening tokens.
 * The rule tree it builds is what the cascade indexes (cascade.c).
 * Recursion follows the nesting of rules in the sheet, limited to
 * MAX_RULE_DEPTH; deeper rules are dropped. */
#include "css_int.h"

#include "../codec/codec.h"

#define MAX_RULE_DEPTH 24

typedef struct {
    const CssTokens *tk;
    AuCssSheet *sheet;
    bool quirks;
    bool imports_allowed;
    int depth;
    bool lazy; /* blocks of declarations and top-level @media blocks stay text until used */
    uint32_t src_base; /* where the tokenized text starts in the sheet's text */
    AuCssMediaList *media[64]; /* distinct media lists so far (media_list) */
    uint32_t nmedia;
} Parser;

bool css_lazy_bodies = true;
bool css_pack_at_parse = false;

static uint32_t late_parses;
uint32_t au_css_late_parse_count(void) { return late_parses; }


static bool is_open(int t) { return t == TK_FUNCTION || t == TK_LPAREN || t == TK_LBRACKET || t == TK_LBRACE; }

/* Index after the component value at i. */
static uint32_t skip_cv(const CssTokens *tk, uint32_t i)
{
    return is_open(tk->t[i].type) ? tk->t[i].len : i + 1;
}

static uint32_t skip_ws(const CssTokens *tk, uint32_t i, uint32_t b)
{
    while (i < b && tk->t[i].type == TK_WS)
        i++;
    return i;
}

/* Contents range of a block or function at i: [start, end). */
static void block_range(const CssTokens *tk, uint32_t i, uint32_t *start, uint32_t *end)
{
    uint32_t e = tk->t[i].len;
    *start = i + 1;
    *end = (e > i + 1 && css_tok_is_close(tk->t[e - 1].type)) ? e - 1 : e;
}

static CssRule *new_rule(Parser *p, int type)
{
    CssRule *r = css_arena_alloc_ptr(&p->sheet->arena, sizeof *r);
    memset(r, 0, sizeof *r);
    r->type = (uint8_t)type;
    r->flags = RF_OK;
    return r;
}

static char *arena_text(Parser *p, char *heap)
{
    char *t = css_arena_strndup(&p->sheet->arena, heap ? heap : "", heap ? strlen(heap) : 0);
    au_free(heap);
    return t;
}

/* ---- The sheet's text, packed ----
 * A sheet's text is read again only for the blocks that are parsed late
 * and by the CSSOM, so once the document has loaded it is kept deflated:
 * in pieces of CSS_TEXT_PIECE bytes, each a raw deflate stream of its
 * own, with the offset of each piece. One inflater and the piece
 * inflated last are shared by all sheets (rules that match together are
 * often next to each other in a sheet). */
#define TEXT_PACK_MIN 4096u
static struct {
    AuInflate *z;
    uint32_t sheets; /* packed sheets alive: the inflater is freed with the last */
    const AuCssSheet *sheet;
    uint32_t piece;
    char data[CSS_TEXT_PIECE];
} packed;

/* Inflates piece i of sh into dst (CSS_TEXT_PIECE bytes of room). */
static void inflate_piece(const AuCssSheet *sh, uint32_t i, char *dst)
{
    size_t want = i + 1 < sh->nz ? CSS_TEXT_PIECE : sh->text_len - (size_t)i * CSS_TEXT_PIECE;
    if (!packed.z)
        packed.z = au_inflate_new(AU_INFLATE_RAW);
    else
        au_inflate_reset(packed.z);
    const uint8_t *in = sh->ztext + sh->zoff[i];
    size_t il = sh->zoff[i + 1] - sh->zoff[i];
    uint8_t *out = (uint8_t *)dst;
    size_t ol = want;
    AuCodecStatus st = AU_CODEC_OK;
    while (st == AU_CODEC_OK && ol) {
        size_t il0 = il, ol0 = ol;
        st = au_inflate_run(packed.z, &in, &il, &out, &ol);
        if (il == il0 && ol == ol0)
            break; /* no progress */
    }
    if (ol)
        memset(out, ' ', ol); /* cannot happen: the stream is our own */
}

const char *css_sheet_text_at(const AuCssSheet *sh, uint32_t off, uint32_t len, AuBuf *tmp)
{
    if (sh->text)
        return sh->text + off;
    if (!sh->ztext || !len)
        return "";
    uint32_t first = off / CSS_TEXT_PIECE, last = (off + len - 1) / CSS_TEXT_PIECE;
    if (first == last) {
        if (packed.sheet != sh || packed.piece != first) {
            inflate_piece(sh, first, packed.data);
            packed.sheet = sh;
            packed.piece = first;
        }
        return packed.data + off % CSS_TEXT_PIECE;
    }
    au_buf_reserve(tmp, (size_t)(last - first + 1) * CSS_TEXT_PIECE);
    for (uint32_t i = first; i <= last; i++)
        inflate_piece(sh, i, tmp->data + (size_t)(i - first) * CSS_TEXT_PIECE);
    return tmp->data + (off - first * CSS_TEXT_PIECE);
}

/* Deflates the text in place. With force, also a small text and one that
 * does not get smaller (tests). */
static bool pack_text(AuCssSheet *s, bool force)
{
    if (!s->text || !s->text_owned || s->ztext || (s->pack_tried && !force))
        return false;
    s->pack_tried = true;
    if (!s->text_len || s->text_len > 0x7FFFFFFFu || (s->text_len < TEXT_PACK_MIN && !force))
        return false;
    uint32_t nz = (uint32_t)((s->text_len + CSS_TEXT_PIECE - 1) / CSS_TEXT_PIECE);
    uint32_t *zoff = au_alloc(((size_t)nz + 1) * sizeof *zoff);
    AuBuf out;
    au_buf_init(&out);
    AuDeflate *z = au_deflate_new(AU_DEFLATE_RAW, 1);
    for (uint32_t i = 0; i < nz; i++) {
        zoff[i] = (uint32_t)out.len;
        const uint8_t *in = (const uint8_t *)s->text + (size_t)i * CSS_TEXT_PIECE;
        size_t il = i + 1 < nz ? CSS_TEXT_PIECE : s->text_len - (size_t)i * CSS_TEXT_PIECE;
        for (;;) {
            au_buf_reserve(&out, 2048);
            uint8_t *op = (uint8_t *)out.data + out.len;
            size_t ol = out.cap - out.len;
            AuCodecStatus st = au_deflate_run(z, &in, &il, &op, &ol, true);
            out.len = (size_t)(op - (uint8_t *)out.data);
            if (st != AU_CODEC_OK)
                break;
        }
        au_deflate_reset(z);
    }
    zoff[nz] = (uint32_t)out.len;
    au_deflate_free(z);
    /* worth it only if it frees an eighth of the text */
    if (!force && out.len + ((size_t)nz + 1) * sizeof *zoff > s->text_len - s->text_len / 8) {
        au_buf_free(&out);
        au_free(zoff);
        return false;
    }
    s->ztext = au_realloc(out.data, out.len ? out.len : 1);
    s->zoff = zoff;
    s->nz = nz;
    au_free(s->text);
    s->text = NULL;
    packed.sheets++;
    return true;
}

bool au_css_sheet_pack_text(AuCssSheet *s) { return pack_text(s, false); }

/* A sheet is freed: its packed text goes, and what was shared for it. */
static void text_unpack(AuCssSheet *s)
{
    if (!s->ztext)
        return;
    au_free(s->ztext);
    au_free(s->zoff);
    s->ztext = NULL;
    if (packed.sheet == s)
        packed.sheet = NULL;
    if (!--packed.sheets && packed.z) {
        au_inflate_free(packed.z);
        packed.z = NULL;
    }
}

/* The block every rule without declarations shares. */
static AuCssDeclBlock empty_block;

/* Moves parsed declarations into the sheet's arena: the block and its
 * array in one allocation that is never grown. */
static AuCssDeclBlock *decls_pack(AuCssSheet *sh, AuCssDeclBlock *tmp)
{
    if (!tmp->n) {
        au_free(tmp->d);
        memset(tmp, 0, sizeof *tmp);
        return &empty_block;
    }
    AuCssDeclBlock *b = css_arena_alloc_ptr(&sh->arena, sizeof *b + tmp->n * sizeof(AuCssDecl));
    b->d = (AuCssDecl *)(b + 1);
    b->n = b->cap = tmp->n;
    memcpy(b->d, tmp->d, tmp->n * sizeof(AuCssDecl));
    au_free(tmp->d);
    memset(tmp, 0, sizeof *tmp);
    return b;
}

typedef struct {
    CssRule **tail;
} RuleList;

static void append_rule(RuleList *l, CssRule *r)
{
    *l->tail = r;
    l->tail = &r->next;
}

/* ---- Declarations ---- */

static bool ident_eq(const CssTokens *tk, const CssToken *t, const char *s)
{
    return t->type == TK_IDENT && au_ieq(css_tok_str(tk, t), t->len, s, strlen(s));
}

/* Parses one declaration in [a, b) (from the name to before the ';').
 * Returns false if it is not a declaration. */
static bool parse_decl(const CssTokens *tk, uint32_t a, uint32_t b, AuCssDeclBlock *out, bool quirks,
                       const char *base_url, bool allow_important)
{
    a = skip_ws(tk, a, b);
    if (a >= b || tk->t[a].type != TK_IDENT)
        return false;
    const CssToken *name = &tk->t[a];
    uint32_t i = skip_ws(tk, a + 1, b);
    if (i >= b || tk->t[i].type != TK_COLON)
        return false;
    i++;
    /* !important at the end */
    uint32_t e = b;
    while (e > i && tk->t[e - 1].type == TK_WS)
        e--;
    bool important = false;
    if (e > i && ident_eq(tk, &tk->t[e - 1], "important")) {
        uint32_t k = e - 1;
        while (k > i && tk->t[k - 1].type == TK_WS)
            k--;
        if (k > i && tk->t[k - 1].type == TK_DELIM && tk->t[k - 1].num == '!') {
            important = true;
            e = k - 1;
        }
    }
    if (important && !allow_important)
        return true; /* ignored, as in keyframes */
    const char *nm = css_tok_str(tk, name);
    if (css_ident_is_custom_prop(nm, name->len)) {
        uint32_t s = skip_ws(tk, i, e);
        while (e > s && tk->t[e - 1].type == TK_WS)
            e--;
        AuCssDecl *d = CSS_PUSH(out->d, out->n, out->cap);
        memset(d, 0, sizeof *d);
        d->prop = AU_PROP_CUSTOM;
        d->kind = DK_CUSTOM;
        d->flags = important ? DF_IMPORTANT : 0;
        d->aux = au_atom(nm, name->len);
        d->u.toks = css_toklist_new(tk, s, e);
        return true;
    }
    uint16_t prop = css_prop_lookup(nm, name->len);
    if (prop && !css_parse_property(prop, tk, i, e, important, out, quirks, base_url) && !quirks) {
        /* A sheet does not know its document's mode when parsed: keep
         * values valid only in quirks mode (unitless lengths), marked so
         * the cascade uses them for quirks-mode documents only. */
        uint32_t before = out->n;
        if (css_parse_property(prop, tk, i, e, important, out, true, base_url))
            for (uint32_t k = before; k < out->n; k++)
                out->d[k].flags |= DF_QUIRKS_ONLY;
    }
    return true;
}

void css_parse_decl_list(const CssTokens *tk, uint32_t a, uint32_t b, AuCssDeclBlock *out, bool quirks,
                         const char *base_url)
{
    uint32_t i = a;
    while (i < b) {
        int ty = tk->t[i].type;
        if (ty == TK_WS || ty == TK_SEMI) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < b && tk->t[j].type != TK_SEMI)
            j = skip_cv(tk, j);
        if (j > b)
            j = b;
        if (ty == TK_IDENT)
            parse_decl(tk, i, j, out, quirks, base_url, true);
        i = j;
    }
}

/* ---- Layer names ---- */

/* Parses "a.b.c" starting at *i; returns false on a syntax error. */
static bool parse_layer_name(Parser *p, uint32_t *i, uint32_t b, CssLayerName *out)
{
    const CssTokens *tk = p->tk;
    AuAtom parts[16];
    uint32_t n = 0;
    uint32_t k = *i;
    if (k >= b || tk->t[k].type != TK_IDENT)
        return false;
    for (;;) {
        const CssToken *t = &tk->t[k];
        if (t->type != TK_IDENT || n >= 16)
            return false;
        parts[n++] = au_atom(css_tok_str(tk, t), t->len);
        k++;
        if (k < b && tk->t[k].type == TK_DELIM && tk->t[k].num == '.' && k + 1 < b && tk->t[k + 1].type == TK_IDENT) {
            k++;
            continue;
        }
        break;
    }
    out->n = n;
    out->parts = css_arena_alloc_ptr(&p->sheet->arena, n * sizeof(AuAtom));
    memcpy(out->parts, parts, n * sizeof(AuAtom));
    *i = k;
    return true;
}

/* ---- Rules ---- */

static void parse_rule_list(Parser *p, uint32_t a, uint32_t b, RuleList *out, bool top);
static void parse_block_contents(Parser *p, uint32_t a, uint32_t b, CssRule *style, AuCssDeclBlock *own,
                                 RuleList *out);

static AuSelList *parse_selectors(Parser *p, uint32_t a, uint32_t b, const AuSelList *parent)
{
    CssSelParseCtx sc = {0};
    sc.arena = &p->sheet->arena;
    sc.parent = parent;
    sc.ns = &p->sheet->ns;
    sc.nested_rule = parent != NULL;
    sc.relative = parent != NULL;
    return css_selector_parse(p->tk, a, b, &sc);
}

/* True if the block at blk, with contents [s, e), is closed by its own }
 * and holds at least one declaration and nothing but declarations: no
 * nested rule and no at-rule. Such a block read again from its source
 * text gives the same tokens, and css_parse_decl_list reads from them
 * what parse_block_contents does. */
static bool only_declarations(const CssTokens *tk, uint32_t blk, uint32_t s, uint32_t e)
{
    if (tk->t[blk].type != TK_LBRACE || tk->t[blk].len != e + 1 || e >= tk->n || tk->t[e].type != TK_RBRACE)
        return false;
    bool any = false;
    uint32_t i = s;
    while (i < e) {
        int ty = tk->t[i].type;
        if (ty == TK_WS || ty == TK_SEMI) {
            i++;
            continue;
        }
        if (ty == TK_AT)
            return false;
        bool custom = ty == TK_IDENT && css_ident_is_custom_prop(css_tok_str(tk, &tk->t[i]), tk->t[i].len);
        uint32_t j = i;
        while (j < e && tk->t[j].type != TK_SEMI) {
            if (tk->t[j].type == TK_LBRACE && !custom)
                return false; /* a nested rule */
            j = skip_cv(tk, j);
        }
        if (j > e)
            return false;
        any = any || ty == TK_IDENT;
        i = j;
    }
    return any;
}

const AuCssDeclBlock *css_rule_decls(const AuCssSheet *sh, const CssRule *ru)
{
    if (!(ru->flags & RF_LAZY))
        return ru->b.decls;
    CssRule *w = (CssRule *)ru; /* the parsed block is a cache of the text */
    late_parses++;
    AuCssDeclBlock tmp = {0};
    CssTokens tk = {0};
    AuBuf text;
    au_buf_init(&text);
    css_tokenize(css_sheet_text_at(sh, w->b.off, w->c.lazy_len, &text), w->c.lazy_len, &tk);
    au_buf_free(&text);
    css_parse_decl_list(&tk, 0, tk.n, &tmp, sh->quirks, sh->base_url);
    css_tokens_free(&tk);
    w->flags &= (uint8_t)~RF_LAZY;
    w->c.children = NULL;
    w->b.decls = decls_pack((AuCssSheet *)sh, &tmp);
    return w->b.decls;
}

static void parse_rule_list(Parser *p, uint32_t a, uint32_t b, RuleList *out, bool top);

const CssRule *css_rule_children(const AuCssSheet *sh, const CssRule *ru)
{
    if (!(ru->flags & RF_LAZY))
        return ru->c.children;
    if (ru->type != RULE_MEDIA)
        return NULL; /* a style rule whose declarations are still text */
    /* A top-level @media block that was kept as text: parse it as the
     * sheet parse would have, one level down. */
    CssRule *w = (CssRule *)ru;
    late_parses++;
    CssTokens tk = {0};
    AuBuf text;
    au_buf_init(&text);
    css_tokenize(css_sheet_text_at(sh, w->b.off, w->c.lazy_len, &text), w->c.lazy_len, &tk);
    au_buf_free(&text);
    Parser p;
    memset(&p, 0, sizeof p);
    p.tk = &tk;
    p.sheet = (AuCssSheet *)sh;
    p.quirks = sh->quirks;
    p.depth = 1;
    p.lazy = true;
    p.src_base = w->b.off;
    w->flags &= (uint8_t)~RF_LAZY;
    w->c.children = NULL;
    w->b.off = 0;
    RuleList inner = {&w->c.children};
    css_toklist_intern_begin();
    css_blob_intern_begin();
    parse_rule_list(&p, 0, tk.n, &inner, true);
    css_blob_intern_end();
    css_toklist_intern_end();
    css_tokens_free(&tk);
    return w->c.children;
}

/* A style rule: prelude [a, pe), block at blk. */
static void parse_style_rule(Parser *p, uint32_t a, uint32_t pe, uint32_t blk, const AuSelList *parent,
                             RuleList *out)
{
    CssArenaMark mark = css_arena_mark(&p->sheet->arena);
    AuSelList *sel = parse_selectors(p, a, pe, parent);
    if (!sel) {
        /* invalid selector: the whole rule is dropped, with what its
         * valid parts took */
        css_arena_release(&p->sheet->arena, mark);
        return;
    }
    CssRule *r = new_rule(p, RULE_STYLE);
    r->u.sel = sel;
    r->b.decls = &empty_block;
    append_rule(out, r);
    p->sheet->nstyle_rules++;
    uint32_t s, e;
    block_range(p->tk, blk, &s, &e);
    if (p->depth >= MAX_RULE_DEPTH)
        return;
    if (p->lazy && only_declarations(p->tk, blk, s, e)) {
        uint32_t off = css_tok_src(&p->tk->t[blk]), end = css_tok_src(&p->tk->t[e]);
        if (end >= off && end <= p->sheet->text_len - p->src_base) {
            r->flags |= RF_LAZY;
            r->b.off = p->src_base + off;
            r->c.lazy_len = end - off;
            return;
        }
    }
    p->depth++;
    AuCssDeclBlock own = {0};
    RuleList children = {&r->c.children};
    parse_block_contents(p, s, e, r, &own, &children);
    r->b.decls = decls_pack(p->sheet, &own);
    p->depth--;
}

/* Ends a run of nested declarations: its block goes to the arena. */
static void nd_close(Parser *p, CssRule **nd, AuCssDeclBlock *ndb)
{
    if (*nd)
        (*nd)->b.decls = decls_pack(p->sheet, ndb);
    *nd = NULL;
}

/* Contents of a style rule's block (or of a conditional rule nested in
 * one): declarations, nested rules, at-rules. Declarations before the
 * first nested rule go to own (the style rule's block; NULL inside a
 * nested conditional rule); later runs of declarations become
 * nested-declarations rules. */
static void parse_block_contents(Parser *p, uint32_t a, uint32_t b, CssRule *style, AuCssDeclBlock *own,
                                 RuleList *out)
{
    const CssTokens *tk = p->tk;
    AuCssDeclBlock *decls = own;
    CssRule *nd = NULL; /* current nested-declarations rule */
    AuCssDeclBlock ndb = {0};
    uint32_t i = a;
    while (i < b) {
        int ty = tk->t[i].type;
        if (ty == TK_WS || ty == TK_SEMI) {
            i++;
            continue;
        }
        if (ty == TK_AT) {
            CssRule **t0 = out->tail;
            uint32_t j = i + 1;
            while (j < b && tk->t[j].type != TK_SEMI && tk->t[j].type != TK_LBRACE)
                j = skip_cv(tk, j);
            bool block = j < b && tk->t[j].type == TK_LBRACE;
            uint32_t end = block ? tk->t[j].len : (j < b ? j + 1 : b);
            if (block) {
                /* a conditional group rule nested in a style rule: its
                 * contents belong to the parent style rule */
                parse_rule_list(p, i, end, out, false);
                CssRule *g = *t0;
                /* a group that never applies (a false @supports, @container,
                 * @scope, @starting-style) keeps no contents */
                if (g && (g->type == RULE_MEDIA || g->type == RULE_LAYER_BLOCK ||
                          (g->type == RULE_SUPPORTS && (g->flags & RF_OK))) &&
                    p->depth < MAX_RULE_DEPTH) {
                    uint32_t s, e;
                    block_range(tk, j, &s, &e);
                    RuleList inner = {&g->c.children};
                    p->depth++;
                    parse_block_contents(p, s, e, style, NULL, &inner);
                    p->depth--;
                }
            }
            decls = NULL;
            nd_close(p, &nd, &ndb);
            i = end;
            continue;
        }
        /* a declaration, or a nested rule if a {} block comes first */
        uint32_t j = i;
        bool has_block = false;
        while (j < b && tk->t[j].type != TK_SEMI) {
            if (tk->t[j].type == TK_LBRACE) {
                has_block = true;
                break;
            }
            j = skip_cv(tk, j);
        }
        if (j > b)
            j = b;
        bool custom = ty == TK_IDENT && css_ident_is_custom_prop(css_tok_str(tk, &tk->t[i]), tk->t[i].len);
        if (has_block && !custom) {
            parse_style_rule(p, i, j, j, style->u.sel, out);
            decls = NULL;
            nd_close(p, &nd, &ndb);
            i = tk->t[j].len;
            continue;
        }
        if (has_block && custom) {
            /* a custom property value may contain {}; it ends at ';' */
            while (j < b && tk->t[j].type != TK_SEMI)
                j = skip_cv(tk, j);
        }
        if (ty == TK_IDENT) {
            AuCssDeclBlock *target = decls;
            if (!target) {
                if (!nd) {
                    nd = new_rule(p, RULE_NESTED_DECLS);
                    nd->u.sel = style->u.sel; /* borrowed */
                    nd->b.decls = &empty_block;
                    append_rule(out, nd);
                }
                target = &ndb;
            }
            parse_decl(tk, i, j, target, p->quirks, p->sheet->base_url, true);
        }
        i = j;
    }
    nd_close(p, &nd, &ndb);
}

/* The media query list in [a, b), in the sheet's arena. A sheet repeats
 * few distinct queries many times: equal ones (by their text) share one
 * list, up to MEDIA_SHARED distinct ones. */
#define MEDIA_SHARED 64
static AuCssMediaList *media_list(Parser *p, uint32_t a, uint32_t b)
{
    AuCssMediaList *m = css_media_parse(p->tk, a, b);
    const char *text = css_media_text(m);
    for (uint32_t i = 0; i < p->nmedia; i++)
        if (!strcmp(css_media_text(p->media[i]), text)) {
            css_media_free(m);
            return p->media[i];
        }
    m = css_media_pack(&p->sheet->arena, m);
    if (p->nmedia < MEDIA_SHARED)
        p->media[p->nmedia++] = m;
    return m;
}

static void add_import(Parser *p, uint32_t a, uint32_t b)
{
    const CssTokens *tk = p->tk;
    uint32_t i = skip_ws(tk, a, b);
    if (i >= b)
        return;
    const CssToken *t = &tk->t[i];
    const char *url = NULL;
    uint32_t ulen = 0;
    if (t->type == TK_STRING || t->type == TK_URL) {
        url = css_tok_str(tk, t);
        ulen = t->len;
        i++;
    } else if (t->type == TK_FUNCTION && t->kw == AU_K_url) {
        uint32_t s, e;
        block_range(tk, i, &s, &e);
        s = skip_ws(tk, s, e);
        if (s >= e || tk->t[s].type != TK_STRING)
            return;
        url = css_tok_str(tk, &tk->t[s]);
        ulen = tk->t[s].len;
        i = t->len;
    } else {
        return;
    }
    AuCssImport imp;
    memset(&imp, 0, sizeof imp);
    imp.supported = true;
    CssLayerName layer = {0};
    bool has_layer = false;
    i = skip_ws(tk, i, b);
    if (i < b && ident_eq(tk, &tk->t[i], "layer")) {
        has_layer = true;
        i++;
    } else if (i < b && tk->t[i].type == TK_FUNCTION && tk->t[i].kw == AU_K_layer) {
        uint32_t s, e;
        block_range(tk, i, &s, &e);
        s = skip_ws(tk, s, e);
        if (!parse_layer_name(p, &s, e, &layer) || skip_ws(tk, s, e) != e)
            return;
        has_layer = true;
        i = tk->t[i].len;
    }
    i = skip_ws(tk, i, b);
    if (i < b && tk->t[i].type == TK_FUNCTION && tk->t[i].kw == AU_K_supports) {
        uint32_t s, e;
        block_range(tk, i, &s, &e);
        AuBuf sb;
        au_buf_init(&sb);
        css_tokens_serialize(tk, s, e, &sb);
        imp.supports = au_buf_take(&sb, NULL);
        /* supports(decl) without parentheses inside is allowed */
        uint32_t k = skip_ws(tk, s, e);
        if (k < e && tk->t[k].type == TK_IDENT) {
            CssTokens tmp = {0};
            AuBuf w;
            au_buf_init(&w);
            au_buf_putc(&w, '(');
            au_buf_puts(&w, imp.supports);
            au_buf_putc(&w, ')');
            css_tokenize(w.data, w.len, &tmp);
            imp.supported = css_supports_eval(&tmp, 0, tmp.n);
            css_tokens_free(&tmp);
            au_buf_free(&w);
        } else {
            imp.supported = css_supports_eval(tk, s, e);
        }
        i = tk->t[i].len;
    }
    i = skip_ws(tk, i, b);
    AuBuf mb;
    au_buf_init(&mb);
    css_tokens_serialize(tk, i, b, &mb);
    while (mb.len && mb.data[mb.len - 1] == ' ')
        mb.len--;
    imp.media = au_buf_take(&mb, NULL);
    imp.url = au_strndup(url, ulen);
    if (has_layer) {
        AuBuf lb;
        au_buf_init(&lb);
        for (uint32_t k = 0; k < layer.n; k++) {
            if (k)
                au_buf_putc(&lb, '.');
            au_buf_puts(&lb, au_atom_str(layer.parts[k]));
        }
        imp.layer = au_buf_take(&lb, NULL);
    }
    AuCssSheet *sh = p->sheet;
    CssRule *r = new_rule(p, RULE_IMPORT);
    r->b.index = sh->nimports;
    r->u.media = media_list(p, i, b);
    if (has_layer)
        r->flags |= RF_LAYER;
    if (!imp.supported)
        r->flags &= (uint8_t)~RF_OK;
    *CSS_PUSH(sh->imports, sh->nimports, sh->imports_cap) = imp;
    sh->import_rules = au_realloc(sh->import_rules, sh->imports_cap * sizeof(CssRule *));
    sh->import_rules[r->b.index] = r; /* the caller appends r to the rule list */
    sh->has_layers = sh->repeat_matters = true;
    sh->import_layers = au_realloc(sh->import_layers, sh->imports_cap * sizeof(CssLayerName));
    sh->import_layers[r->b.index] = layer;
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* One <urange> from its text after the "u" ("+26", "+0-7F", "+4??"):
 * CSS Syntax 3, section 7.1. */
static bool urange_text(const char *t, size_t n, uint32_t *first, uint32_t *last)
{
    size_t i = 0;
    if (i >= n || t[i] != '+')
        return false;
    i++;
    uint32_t v = 0;
    int digits = 0, marks = 0;
    for (; i < n && hex_digit((unsigned char)t[i]) >= 0; i++, digits++)
        v = digits < 6 ? v * 16 + (uint32_t)hex_digit((unsigned char)t[i]) : v;
    for (; i < n && t[i] == '?'; i++)
        marks++;
    if (digits + marks == 0 || digits + marks > 6)
        return false;
    if (marks) {
        if (i != n)
            return false;
        *first = v << (4 * marks);
        *last = *first | ((1u << (4 * marks)) - 1);
    } else if (i == n) {
        *first = *last = v;
    } else {
        if (t[i] != '-')
            return false;
        i++;
        uint32_t w = 0;
        digits = 0;
        for (; i < n && hex_digit((unsigned char)t[i]) >= 0; i++, digits++)
            w = digits < 6 ? w * 16 + (uint32_t)hex_digit((unsigned char)t[i]) : w;
        if (digits == 0 || digits > 6 || i != n)
            return false;
        *first = v;
        *last = w;
    }
    return *last <= 0x10FFFF && *first <= *last;
}

/* The unicode-range descriptor: <urange>#. The tokenizer has no urange
 * token ("u+a" is also a selector), so each range is put together from
 * the text of its tokens: the ident "u", then idents, numbers and
 * dimensions as written, "+" and "?". Returns a COMMA_LIST with one LIST
 * of two integer NUMBERs (first and last code point) per range, or NULL
 * if the value is invalid. */
static AuCssValue *parse_unicode_range(CssArena *arena, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t count = 1;
    for (uint32_t i = a; i < b; i++)
        count += tk->t[i].type == TK_COMMA;
    if (count > 4096)
        return NULL;
    AuCssValue *list = css_new_list(arena, AU_CSSV_COMMA_LIST, count);
    uint32_t item = 0;
    for (uint32_t i = a; i <= b;) {
        uint32_t e = i;
        while (e < b && tk->t[e].type != TK_COMMA)
            e++;
        uint32_t s = skip_ws(tk, i, e);
        while (e > s && tk->t[e - 1].type == TK_WS)
            e--;
        if (s >= e || tk->t[s].type != TK_IDENT || tk->t[s].len != 1 || (css_tok_str(tk, &tk->t[s])[0] | 0x20) != 'u')
            return NULL;
        char text[24];
        size_t n = 0;
        for (uint32_t k = s + 1; k < e; k++) {
            const CssToken *t = &tk->t[k];
            const char *part = NULL;
            uint32_t plen = 0;
            char ch;
            if (t->type == TK_IDENT) {
                part = css_tok_str(tk, t);
                plen = t->len;
            } else if (t->type == TK_DELIM && (t->num == '+' || t->num == '?')) {
                ch = (char)t->num;
                part = &ch;
                plen = 1;
            } else if (t->type == TK_NUMBER || t->type == TK_DIMENSION) {
                part = css_tok_num_text(tk, t, &plen);
                if (!plen)
                    return NULL;
            } else {
                return NULL;
            }
            if (n + plen > sizeof text)
                return NULL;
            memcpy(text + n, part, plen);
            n += plen;
            if (t->type == TK_DIMENSION) {
                if (n + t->len > sizeof text)
                    return NULL;
                memcpy(text + n, css_tok_str(tk, t), t->len);
                n += t->len;
            }
        }
        uint32_t first, last;
        if (!urange_text(text, n, &first, &last))
            return NULL;
        AuCssValue *r = &list->u.items[item++];
        r->type = AU_CSSV_LIST;
        r->n = 2;
        r->u.items = css_arena_alloc(arena, 2 * sizeof(AuCssValue));
        memset(r->u.items, 0, 2 * sizeof(AuCssValue));
        r->u.items[0].type = r->u.items[1].type = AU_CSSV_NUMBER;
        r->u.items[0].unit = r->u.items[1].unit = 1;
        r->u.items[0].u.num = (float)first;
        r->u.items[1].u.num = (float)last;
        if (e >= b)
            break;
        /* e may have been moved back over whitespace: go on after the comma */
        i = e;
        while (i < b && tk->t[i].type != TK_COMMA)
            i++;
        i++;
    }
    return list;
}

static void parse_font_face(Parser *p, uint32_t s, uint32_t e)
{
    const CssTokens *tk = p->tk;
    AuCssSheet *sh = p->sheet;
    AuCssFontFace *f = CSS_PUSH(sh->faces, sh->nfaces, sh->faces_cap);
    memset(f, 0, sizeof *f);
    f->base_url = sh->base_url;
    uint32_t i = s;
    CssArena arena = {0};
    while (i < e) {
        if (tk->t[i].type != TK_IDENT) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < e && tk->t[j].type != TK_SEMI)
            j = skip_cv(tk, j);
        if (j > e)
            j = e;
        uint32_t k = skip_ws(tk, i + 1, j);
        if (k < j && tk->t[k].type == TK_COLON) {
            k++;
            uint32_t ve = j;
            while (ve > k && tk->t[ve - 1].type == TK_WS)
                ve--;
            k = skip_ws(tk, k, ve);
            char name[32];
            if (css_lower(css_tok_str(tk, &tk->t[i]), tk->t[i].len, name, sizeof name) && k < ve) {
                CssParseCtx cx = {&arena, false, false, sh->base_url};
                bool urange = strcmp(name, "unicode-range") == 0;
                AuCssValue *v = urange ? parse_unicode_range(&arena, tk, k, ve) : css_parse_value(&cx, tk, k, ve);
                if (!v && urange) {
                    /* an invalid descriptor is ignored: the face covers every character */
                    css_arena_reset(&arena);
                    i = j;
                    continue;
                }
                if (!v) {
                    AuBuf b;
                    au_buf_init(&b);
                    css_tokens_serialize(tk, k, ve, &b);
                    v = css_new_value(&arena, AU_CSSV_STRING);
                    v->u.str = css_arena_strndup(&arena, b.data ? b.data : "", b.len);
                    v->n = (uint32_t)b.len;
                    au_buf_free(&b);
                }
                AuCssDecl *d = CSS_PUSH(f->descs.d, f->descs.n, f->descs.cap);
                memset(d, 0, sizeof *d);
                d->kind = DK_BLOB;
                d->aux = au_atom_cstr(name);
                d->u.blob = css_blob_pack(v, sh->base_url);
                css_arena_reset(&arena);
            }
        }
        i = j;
    }
    css_arena_free(&arena);
}

static void parse_keyframes(Parser *p, uint32_t a, uint32_t b, uint32_t s, uint32_t e)
{
    const CssTokens *tk = p->tk;
    uint32_t i = skip_ws(tk, a, b);
    if (i >= b || (tk->t[i].type != TK_IDENT && tk->t[i].type != TK_STRING))
        return;
    if (tk->t[i].type == TK_IDENT && (tk->t[i].kw == AU_K_none || tk->t[i].kw == AU_K_initial ||
                                      tk->t[i].kw == AU_K_inherit || tk->t[i].kw == AU_K_unset ||
                                      tk->t[i].kw == AU_K_default_))
        return;
    if (skip_ws(tk, i + 1, b) != b)
        return;
    AuCssSheet *sh = p->sheet;
    AuCssKeyframes *k = CSS_PUSH(sh->keyframes, sh->nkeyframes, sh->keyframes_cap);
    memset(k, 0, sizeof *k);
    k->name = au_atom(css_tok_str(tk, &tk->t[i]), tk->t[i].len);
    uint32_t cap = 0;
    uint32_t j = s;
    while (j < e) {
        if (tk->t[j].type == TK_WS || tk->t[j].type == TK_SEMI) {
            j++;
            continue;
        }
        uint32_t q = j;
        while (q < e && tk->t[q].type != TK_LBRACE)
            q = skip_cv(tk, q);
        if (q >= e)
            break;
        /* selector: from | to | <percentage>, comma separated */
        float offs[32];
        uint32_t no = 0;
        bool ok = true;
        bool want = true;
        for (uint32_t x = j; x < q && ok; x++) {
            const CssToken *t = &tk->t[x];
            if (t->type == TK_WS)
                continue;
            if (t->type == TK_COMMA && !want) {
                want = true;
                continue;
            }
            if (!want || no >= 32) {
                ok = false;
                break;
            }
            if (ident_eq(tk, t, "from"))
                offs[no++] = 0;
            else if (ident_eq(tk, t, "to"))
                offs[no++] = 1;
            else if (t->type == TK_PERCENT && t->num >= 0 && t->num <= 100)
                offs[no++] = t->num / 100;
            else
                ok = false;
            want = false;
        }
        uint32_t bs, be;
        block_range(tk, q, &bs, &be);
        if (ok && no && !want) {
            AuCssKeyframe *f = CSS_PUSH(k->frames, k->nframes, cap);
            f->offsets = au_alloc(no * sizeof(float));
            memcpy(f->offsets, offs, no * sizeof(float));
            f->noffsets = no;
            f->decls = au_calloc(sizeof(AuCssDeclBlock));
            uint32_t x = bs;
            while (x < be) {
                if (tk->t[x].type != TK_IDENT) {
                    x++;
                    continue;
                }
                uint32_t y = x;
                while (y < be && tk->t[y].type != TK_SEMI)
                    y = skip_cv(tk, y);
                if (y > be)
                    y = be;
                parse_decl(tk, x, y, f->decls, p->quirks, sh->base_url, false);
                x = y;
            }
        }
        j = tk->t[q].len;
    }
}

static void parse_property_rule(Parser *p, uint32_t a, uint32_t b, uint32_t s, uint32_t e)
{
    const CssTokens *tk = p->tk;
    uint32_t i = skip_ws(tk, a, b);
    if (i >= b || tk->t[i].type != TK_IDENT || !css_ident_is_custom_prop(css_tok_str(tk, &tk->t[i]), tk->t[i].len))
        return;
    AuCssSheet *sh = p->sheet;
    CssPropertyRule pr = {au_atom(css_tok_str(tk, &tk->t[i]), tk->t[i].len), true, NULL};
    bool has_syntax = false, has_inherits = false;
    uint32_t x = s;
    while (x < e) {
        if (tk->t[x].type != TK_IDENT) {
            x++;
            continue;
        }
        uint32_t y = x;
        while (y < e && tk->t[y].type != TK_SEMI)
            y = skip_cv(tk, y);
        if (y > e)
            y = e;
        uint32_t k = skip_ws(tk, x + 1, y);
        if (k < y && tk->t[k].type == TK_COLON) {
            uint32_t vs = skip_ws(tk, k + 1, y), ve = y;
            while (ve > vs && tk->t[ve - 1].type == TK_WS)
                ve--;
            if (ident_eq(tk, &tk->t[x], "syntax"))
                has_syntax = true;
            else if (ident_eq(tk, &tk->t[x], "inherits") && vs < ve) {
                has_inherits = true;
                pr.inherits = ident_eq(tk, &tk->t[vs], "true");
            } else if (ident_eq(tk, &tk->t[x], "initial-value")) {
                if (pr.initial)
                    css_toklist_release(pr.initial);
                pr.initial = css_toklist_new(tk, vs, ve);
            }
        }
        x = y;
    }
    if (!has_syntax || !has_inherits) {
        css_toklist_release(pr.initial);
        return;
    }
    *CSS_PUSH(sh->props, sh->nprops, sh->props_cap) = pr;
}

static void add_namespace(Parser *p, uint32_t a, uint32_t b)
{
    const CssTokens *tk = p->tk;
    uint32_t i = skip_ws(tk, a, b);
    AuAtom prefix = 0;
    if (i < b && tk->t[i].type == TK_IDENT) {
        prefix = au_atom(css_tok_str(tk, &tk->t[i]), tk->t[i].len);
        i = skip_ws(tk, i + 1, b);
    }
    if (i >= b)
        return;
    const char *url = NULL;
    uint32_t ulen = 0;
    if (tk->t[i].type == TK_STRING || tk->t[i].type == TK_URL) {
        url = css_tok_str(tk, &tk->t[i]);
        ulen = tk->t[i].len;
    } else if (tk->t[i].type == TK_FUNCTION && tk->t[i].kw == AU_K_url) {
        uint32_t s, e;
        block_range(tk, i, &s, &e);
        s = skip_ws(tk, s, e);
        if (s < e && tk->t[s].type == TK_STRING) {
            url = css_tok_str(tk, &tk->t[s]);
            ulen = tk->t[s].len;
        }
    }
    if (!url)
        return;
    int ns = -2; /* unknown namespace: matches nothing */
    if (au_ieq(url, ulen, "http://www.w3.org/1999/xhtml", 28))
        ns = AU_NS_HTML;
    else if (au_ieq(url, ulen, "http://www.w3.org/2000/svg", 26))
        ns = AU_NS_SVG;
    else if (au_ieq(url, ulen, "http://www.w3.org/1998/Math/MathML", 34))
        ns = AU_NS_MATHML;
    CssNamespaces *n = &p->sheet->ns;
    if (!prefix) {
        n->default_ns = ns;
        return;
    }
    css_grow((void **)&n->prefixes, &n->cap, n->n + 1, sizeof(AuAtom));
    n->nss = au_realloc(n->nss, n->cap * sizeof(int));
    n->prefixes[n->n] = prefix;
    n->nss[n->n] = ns;
    n->n++;
}

static char *range_text(const CssTokens *tk, uint32_t a, uint32_t b)
{
    AuBuf buf;
    au_buf_init(&buf);
    a = skip_ws(tk, a, b);
    while (b > a && tk->t[b - 1].type == TK_WS)
        b--;
    css_tokens_serialize(tk, a, b, &buf);
    return au_buf_take(&buf, NULL);
}

/* True if the block at blk, with contents [s, e), is closed by its own }
 * and can be parsed later from its text with the same result: it holds
 * no rule that registers something with the sheet when parsed
 * (@font-face, @keyframes, @property). */
static bool block_can_wait(Parser *p, uint32_t blk, uint32_t s, uint32_t e)
{
    const CssTokens *tk = p->tk;
    if (tk->t[blk].type != TK_LBRACE || tk->t[blk].len != e + 1 || e >= tk->n || tk->t[e].type != TK_RBRACE)
        return false;
    for (uint32_t i = s; i < e; i++) {
        if (tk->t[i].type != TK_AT)
            continue;
        const char *name = css_tok_str(tk, &tk->t[i]);
        uint32_t nlen = tk->t[i].len;
        if (au_ieq(name, nlen, "layer", 5)) /* which may be anonymous: known before the block is parsed */
            p->sheet->has_layers = p->sheet->repeat_matters = true;
        if (au_ieq(name, nlen, "font-face", 9) || au_ieq(name, nlen, "keyframes", 9) ||
            au_ieq(name, nlen, "-webkit-keyframes", 17) || au_ieq(name, nlen, "-moz-keyframes", 14) ||
            au_ieq(name, nlen, "property", 8))
            return false;
    }
    return true;
}

/* One at-rule at i, ending before b. Returns the index after it. In nested
 * mode (top == false, inside a style rule) group rules get no children
 * here; parse_block_contents fills them. */
static uint32_t parse_at_rule(Parser *p, uint32_t i, uint32_t b, RuleList *out, bool top)
{
    const CssTokens *tk = p->tk;
    const CssToken *at = &tk->t[i];
    uint32_t j = i + 1;
    while (j < b && tk->t[j].type != TK_SEMI && tk->t[j].type != TK_LBRACE)
        j = skip_cv(tk, j);
    bool block = j < b && tk->t[j].type == TK_LBRACE;
    uint32_t end = block ? tk->t[j].len : (j < b ? j + 1 : b);
    uint32_t pa = i + 1, pe = j > b ? b : j;
    uint32_t bs = 0, be = 0;
    if (block)
        block_range(tk, j, &bs, &be);
    const char *name = css_tok_str(tk, at);
    uint32_t nlen = at->len;
#define IS(s) au_ieq(name, nlen, s, sizeof(s) - 1)
    if (IS("charset")) {
        return end;
    }
    if (IS("import")) {
        /* only at the top of the sheet, before its rules */
        if (!block && top && p->depth == 0 && p->imports_allowed) {
            uint32_t before = p->sheet->nimports;
            add_import(p, pa, pe);
            if (p->sheet->nimports > before)
                append_rule(out, p->sheet->import_rules[before]);
        }
        return end;
    }
    if (IS("namespace")) {
        /* only at the top of the sheet, before its rules: every selector
         * of the sheet is parsed with the same prefixes */
        if (!block && top && p->depth == 0 && p->imports_allowed)
            add_namespace(p, pa, pe);
        return end;
    }
    if (IS("layer")) {
        p->sheet->has_layers = true;
        /* statement: names, comma separated; block: optional name */
        uint32_t k = skip_ws(tk, pa, pe);
        CssLayerName names[32];
        uint32_t n = 0;
        bool ok = true;
        while (k < pe && ok) {
            if (n >= 32 || !parse_layer_name(p, &k, pe, &names[n])) {
                ok = false;
                break;
            }
            n++;
            k = skip_ws(tk, k, pe);
            if (k < pe) {
                if (tk->t[k].type != TK_COMMA) {
                    ok = false;
                    break;
                }
                k = skip_ws(tk, k + 1, pe);
                if (k >= pe)
                    ok = false;
            }
        }
        if (!ok || (block && n > 1) || (!block && n == 0))
            return end;
        if (block && n == 0)
            p->sheet->repeat_matters = true; /* an anonymous layer */
        CssRule *r = new_rule(p, block ? RULE_LAYER_BLOCK : RULE_LAYER_STMT);
        r->b.nlayers = block ? 1 : n;
        r->u.layers = css_arena_alloc_ptr(&p->sheet->arena, r->b.nlayers * sizeof(CssLayerName));
        memset(r->u.layers, 0, r->b.nlayers * sizeof(CssLayerName));
        if (n)
            memcpy(r->u.layers, names, n * sizeof(CssLayerName));
        append_rule(out, r);
        if (block && top && p->depth < MAX_RULE_DEPTH) {
            p->depth++;
            RuleList inner = {&r->c.children};
            parse_rule_list(p, bs, be, &inner, true);
            p->depth--;
        }
        return end;
    }
    if (!block)
        return end; /* other statement at-rules are unknown */
    if (IS("media")) {
        CssRule *r = new_rule(p, RULE_MEDIA);
        r->u.media = media_list(p, pa, pe);
        append_rule(out, r);
        if (top && p->depth == 0 && p->lazy && block_can_wait(p, j, bs, be)) {
            uint32_t off = css_tok_src(&tk->t[j]), e2 = css_tok_src(&tk->t[be]);
            if (e2 >= off && e2 <= p->sheet->text_len - p->src_base) {
                r->flags |= RF_LAZY;
                r->b.off = p->src_base + off;
                r->c.lazy_len = e2 - off;
                return end;
            }
        }
        if (top && p->depth < MAX_RULE_DEPTH) {
            p->depth++;
            RuleList inner = {&r->c.children};
            parse_rule_list(p, bs, be, &inner, true);
            p->depth--;
        }
        return end;
    }
    if (IS("supports")) {
        CssRule *r = new_rule(p, RULE_SUPPORTS);
        if (!css_supports_eval(tk, pa, pe))
            r->flags &= (uint8_t)~RF_OK;
        r->u.text = arena_text(p, range_text(tk, pa, pe));
        append_rule(out, r);
        /* the contents of a false @supports are not kept */
        if (top && (r->flags & RF_OK) && p->depth < MAX_RULE_DEPTH) {
            p->depth++;
            RuleList inner = {&r->c.children};
            parse_rule_list(p, bs, be, &inner, true);
            p->depth--;
        }
        return end;
    }
    if (IS("container") || IS("scope") || IS("starting-style")) {
        /* kept as a rule without contents: container queries, @scope and
         * @starting-style are not evaluated, so their rules never apply */
        CssRule *r = new_rule(p, IS("container") ? RULE_CONTAINER : IS("scope") ? RULE_SCOPE : RULE_STARTING_STYLE);
        r->flags &= (uint8_t)~RF_OK;
        r->u.text = arena_text(p, range_text(tk, pa, pe));
        append_rule(out, r);
        return end;
    }
    if (!top)
        return end;
    if (IS("font-face")) {
        parse_font_face(p, bs, be);
        return end;
    }
    if (IS("keyframes") || IS("-webkit-keyframes") || IS("-moz-keyframes")) {
        parse_keyframes(p, pa, pe, bs, be);
        return end;
    }
    if (IS("property")) {
        parse_property_rule(p, pa, pe, bs, be);
        return end;
    }
#undef IS
    return end; /* @page, @counter-style, unknown: skipped */
}

static void parse_rule_list(Parser *p, uint32_t a, uint32_t b, RuleList *out, bool top)
{
    const CssTokens *tk = p->tk;
    uint32_t i = a;
    if (!top) {
        /* nested at-rule: parse just the at-rule at a */
        parse_at_rule(p, a, b, out, false);
        return;
    }
    while (i < b) {
        int ty = tk->t[i].type;
        if (ty == TK_WS || ty == TK_CDO || ty == TK_CDC) {
            i++;
            continue;
        }
        if (ty == TK_AT) {
            bool is_import = au_ieq(css_tok_str(tk, &tk->t[i]), tk->t[i].len, "import", 6);
            bool keeps = is_import || au_ieq(css_tok_str(tk, &tk->t[i]), tk->t[i].len, "charset", 7) ||
                         au_ieq(css_tok_str(tk, &tk->t[i]), tk->t[i].len, "namespace", 9);
            bool layer_stmt = false;
            if (au_ieq(css_tok_str(tk, &tk->t[i]), tk->t[i].len, "layer", 5)) {
                uint32_t j = i + 1;
                while (j < b && tk->t[j].type != TK_SEMI && tk->t[j].type != TK_LBRACE)
                    j = skip_cv(tk, j);
                layer_stmt = !(j < b && tk->t[j].type == TK_LBRACE);
            }
            i = parse_at_rule(p, i, b, out, true);
            if (!keeps && !layer_stmt)
                p->imports_allowed = false;
            continue;
        }
        p->imports_allowed = false;
        /* qualified rule: prelude up to a {} block */
        uint32_t j = i;
        while (j < b && tk->t[j].type != TK_LBRACE)
            j = skip_cv(tk, j);
        if (j >= b)
            return; /* EOF in the prelude: dropped */
        parse_style_rule(p, i, j, j, NULL, out);
        i = tk->t[j].len;
    }
}

AuCssSheet *css_sheet_parse(const char *css, size_t len, AuCssOrigin origin, const char *base_url,
                            const char *media, bool quirks, int text_mode)
{
    au_css_init();
    AuCssSheet *sh = au_calloc(sizeof *sh);
    sh->refs = 1;
    sh->origin = (uint8_t)origin;
    sh->quirks = quirks;
    sh->base_url = base_url ? au_strndup(base_url, strlen(base_url)) : NULL;
    sh->ns.default_ns = -1;
    /* chunks double up to 16 KB, or to the size of the text for a sheet
     * smaller than that, so that a small sheet's last chunk is small */
    sh->arena.chunk = 496;
    sh->arena.chunk_max = len < 496 ? 496 : len > 16368 ? 16368 : (len + 15) & ~(size_t)15;
    if (media && *media)
        sh->media = css_media_parse_text(media, strlen(media));
    CssTokens tk = {0};
    css_tokenize(css, len, &tk);
    /* The text stays with the sheet (a brace token holds a 32-bit offset
     * into it, so not a sheet of 4 GB). */
    if (len < 0xFFFFFFFFu) {
        if (text_mode == CSS_TEXT_COPY) {
            sh->text = au_alloc(len + 1);
            if (len)
                memcpy(sh->text, css, len);
            sh->text[len] = 0;
        } else {
            sh->text = (char *)css;
        }
        sh->text_owned = text_mode != CSS_TEXT_BORROW;
        sh->text_len = len;
        sh->has_text = true;
    }
    Parser p;
    memset(&p, 0, sizeof p);
    p.tk = &tk;
    p.sheet = sh;
    p.quirks = quirks;
    p.imports_allowed = true;
    p.lazy = css_lazy_bodies && sh->has_text;
    RuleList l = {&sh->rules};
    /* equal var() and custom property values share one token list, equal
     * declared values one blob */
    css_toklist_intern_begin();
    css_blob_intern_begin();
    parse_rule_list(&p, 0, tk.n, &l, true);
    css_blob_intern_end();
    css_toklist_intern_end();
    css_tokens_free(&tk);
    if (!sh->has_text && text_mode == CSS_TEXT_TAKE)
        au_free((char *)css);
    if (css_pack_at_parse)
        pack_text(sh, true);
    return sh;
}

AuCssSheet *au_css_sheet_parse(const char *css, size_t len, AuCssOrigin origin, const char *base_url,
                               const char *media)
{
    return css_sheet_parse(css, len, origin, base_url, media, false, CSS_TEXT_COPY);
}

AuCssSheet *au_css_sheet_parse_take(char *css, size_t len, AuCssOrigin origin, const char *base_url,
                                    const char *media)
{
    return css_sheet_parse(css, len, origin, base_url, media, false, CSS_TEXT_TAKE);
}

size_t au_css_sheet_memory(const AuCssSheet *s)
{
    size_t bytes, used, chunks;
    css_arena_mem(&s->arena, &bytes, &used, &chunks);
    size_t text = s->ztext ? s->zoff[s->nz] + (s->nz + 1) * sizeof(uint32_t) : s->text && s->text_owned ? s->text_len : 0;
    return sizeof *s + bytes + text;
}

bool au_css_sheet_text_get(const AuCssSheet *s, AuBuf *out)
{
    if (!s->has_text)
        return false;
    for (size_t off = 0; off < s->text_len;) {
        size_t n = s->text_len - off;
        if (s->ztext && n > CSS_TEXT_PIECE)
            n = CSS_TEXT_PIECE; /* one piece at a time */
        AuBuf tmp;
        au_buf_init(&tmp);
        au_buf_append(out, css_sheet_text_at(s, (uint32_t)off, (uint32_t)n, &tmp), n);
        au_buf_free(&tmp);
        off += n;
    }
    return true;
}

bool au_css_sheet_text_is(const AuCssSheet *s, const char *text, size_t len)
{
    if (!s->has_text || s->text_len != len)
        return false;
    for (size_t off = 0; off < len;) {
        size_t n = len - off;
        if (s->ztext && n > CSS_TEXT_PIECE)
            n = CSS_TEXT_PIECE;
        AuBuf tmp;
        au_buf_init(&tmp);
        bool same = memcmp(css_sheet_text_at(s, (uint32_t)off, (uint32_t)n, &tmp), text + off, n) == 0;
        au_buf_free(&tmp);
        if (!same)
            return false;
        off += n;
    }
    return true;
}

/* ---- Freeing ---- */

/* Lets go of what the declarations of the rules refer to (value blobs,
 * token lists). The rules and their arrays are in the sheet's arena. */
static void rules_release(CssRule *r)
{
    /* iterative: children are spliced in front of the remaining list */
    while (r) {
        if (!(r->flags & RF_LAZY) && r->c.children) {
            CssRule *last = r->c.children;
            while (last->next)
                last = last->next;
            last->next = r->next;
            r->next = r->c.children;
            r->c.children = NULL;
        }
        if ((r->type == RULE_STYLE || r->type == RULE_NESTED_DECLS) && !(r->flags & RF_LAZY))
            for (uint32_t i = 0; i < r->b.decls->n; i++)
                css_decl_release(&r->b.decls->d[i]);
        r = r->next;
    }
}

AuCssSheet *au_css_sheet_retain(AuCssSheet *s)
{
    if (s)
        s->refs++;
    return s;
}

void au_css_sheet_free(AuCssSheet *s)
{
    if (!s || --s->refs)
        return;
    rules_release(s->rules);
    css_arena_free(&s->arena);
    au_free(s->import_layers);
    for (uint32_t i = 0; i < s->nimports; i++) {
        AuCssImport *im = &s->imports[i];
        au_free((char *)im->url);
        au_free((char *)im->media);
        au_free((char *)im->layer);
        au_free((char *)im->supports);
        au_css_sheet_free(im->sheet);
    }
    au_free(s->imports);
    au_free(s->import_rules);
    for (uint32_t i = 0; i < s->nfaces; i++)
        css_decl_block_clear(&s->faces[i].descs);
    au_free(s->faces);
    for (uint32_t i = 0; i < s->nkeyframes; i++) {
        AuCssKeyframes *k = &s->keyframes[i];
        for (uint32_t j = 0; j < k->nframes; j++) {
            au_free(k->frames[j].offsets);
            css_decl_block_clear(k->frames[j].decls);
            au_free(k->frames[j].decls);
        }
        au_free(k->frames);
    }
    au_free(s->keyframes);
    for (uint32_t i = 0; i < s->nprops; i++)
        css_toklist_release(s->props[i].initial);
    au_free(s->props);
    au_free(s->ns.prefixes);
    au_free(s->ns.nss);
    if (s->media)
        css_media_free(s->media);
    au_free(s->base_url);
    if (s->text_owned)
        au_free(s->text);
    text_unpack(s);
    au_free(s);
}

size_t au_css_sheet_rule_count(const AuCssSheet *s)
{
    /* The style rules of @media blocks that are still text count too:
     * parse them. Rules nest at most MAX_RULE_DEPTH deep. */
    const CssRule *stack[MAX_RULE_DEPTH + 4];
    uint32_t sp = 0;
    const CssRule *r = s->rules;
    for (;;) {
        if (!r) {
            if (!sp)
                break;
            r = stack[--sp];
            continue;
        }
        const CssRule *kids = css_rule_children(s, r);
        if (kids && sp < CSS_ARRAY_LEN(stack)) {
            stack[sp++] = r->next;
            r = kids;
        } else {
            r = r->next;
        }
    }
    return s->nstyle_rules;
}
size_t au_css_sheet_import_count(const AuCssSheet *s) { return s->nimports; }
const AuCssImport *au_css_sheet_import(const AuCssSheet *s, size_t i) { return i < s->nimports ? &s->imports[i] : NULL; }

void au_css_sheet_set_import(AuCssSheet *s, size_t i, AuCssSheet *child)
{
    if (i >= s->nimports) {
        au_css_sheet_free(child);
        return;
    }
    au_css_sheet_free(s->imports[i].sheet);
    s->imports[i].sheet = child;
}

size_t au_css_sheet_font_face_count(const AuCssSheet *s) { return s->nfaces; }
const AuCssFontFace *au_css_sheet_font_face(const AuCssSheet *s, size_t i) { return i < s->nfaces ? &s->faces[i] : NULL; }

const AuCssValue *au_css_font_face_descriptor(const AuCssFontFace *f, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    if (!a)
        return NULL;
    for (uint32_t i = f->descs.n; i > 0; i--)
        if (f->descs.d[i - 1].aux == a)
            return &f->descs.d[i - 1].u.blob->value;
    return NULL;
}

const char *au_css_font_face_base_url(const AuCssFontFace *f) { return f->base_url; }

/* ---- Inline style ---- */

AuCssDeclBlock *au_css_parse_inline_style(const char *css, size_t len, const char *base_url)
{
    au_css_init();
    AuCssDeclBlock *b = au_calloc(sizeof *b);
    CssTokens tk = {0};
    css_tokenize(css, len, &tk);
    css_parse_decl_list(&tk, 0, tk.n, b, false, base_url);
    css_tokens_free(&tk);
    return b;
}

void au_css_decl_block_free(AuCssDeclBlock *b)
{
    if (!b)
        return;
    css_decl_block_clear(b);
    au_free(b);
}

size_t au_css_decl_block_count(const AuCssDeclBlock *b) { return b ? b->n : 0; }
