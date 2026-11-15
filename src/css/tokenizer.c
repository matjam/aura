/* CSS tokenizer, per CSS Syntax Level 3 section 4.
 *
 * css_tokenize turns the whole input into a flat token array. Strings
 * (identifiers, unescaped string contents, url contents, units) go to a
 * pool, NUL-terminated. After tokenizing, every opening token (function,
 * "(", "[", "{") records in len the index just past its matching closing
 * token, so the parser can skip a component value in one step.
 * Comments produce no token. */
#include "css_int.h"

/* ---- Helpers shared across src/css ---- */

void css_grow(void **arr, uint32_t *cap, uint32_t n, size_t sz)
{
    if (n <= *cap)
        return;
    uint32_t c = *cap ? *cap : 4;
    while (c < n)
        c *= 2;
    *arr = au_realloc(*arr, (size_t)c * sz);
    *cap = c;
}

bool css_lower(const char *s, size_t n, char *buf, size_t cap)
{
    if (n + 1 > cap)
        return false;
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)au_lower((unsigned char)s[i]);
    buf[n] = 0;
    return true;
}

/* ---- Arena ---- */

struct CssArenaChunk {
    CssArenaChunk *next;
    size_t cap;
    /* data follows, aligned */
};
#define ARENA_HDR ((sizeof(CssArenaChunk) + 15) & ~(size_t)15)

void *css_arena_alloc(CssArena *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (!a->head || a->used + n > a->cap) {
        size_t cap = n > 8192 ? n : 8192;
        CssArenaChunk *c = au_alloc(ARENA_HDR + cap);
        c->cap = cap;
        c->next = a->head;
        a->head = c;
        a->used = 0;
        a->cap = cap;
    }
    void *p = (char *)a->head + ARENA_HDR + a->used;
    a->used += n;
    return p;
}

char *css_arena_strndup(CssArena *a, const char *s, size_t n)
{
    char *p = css_arena_alloc(a, n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void css_arena_reset(CssArena *a)
{
    if (!a->head)
        return;
    /* keep the last chunk in the chain (the first allocated) */
    CssArenaChunk *c = a->head;
    while (c->next) {
        CssArenaChunk *n = c->next;
        au_free(c);
        c = n;
    }
    a->head = c;
    a->cap = c->cap;
    a->used = 0;
}

void css_arena_free(CssArena *a)
{
    CssArenaChunk *c = a->head;
    while (c) {
        CssArenaChunk *n = c->next;
        au_free(c);
        c = n;
    }
    a->head = NULL;
    a->used = a->cap = 0;
}

/* ---- Units ---- */

static const struct {
    const char *name;
    uint8_t unit;
} unit_names[] = {
    {"px", AU_UNIT_PX},     {"cm", AU_UNIT_CM},       {"mm", AU_UNIT_MM},       {"q", AU_UNIT_Q},
    {"in", AU_UNIT_IN},     {"pt", AU_UNIT_PT},       {"pc", AU_UNIT_PC},       {"em", AU_UNIT_EM},
    {"rem", AU_UNIT_REM},   {"ex", AU_UNIT_EX},       {"rex", AU_UNIT_REX},     {"ch", AU_UNIT_CH},
    {"rch", AU_UNIT_RCH},   {"cap", AU_UNIT_CAP},     {"rcap", AU_UNIT_RCAP},   {"ic", AU_UNIT_IC},
    {"ric", AU_UNIT_RIC},   {"lh", AU_UNIT_LH},       {"rlh", AU_UNIT_RLH},     {"vw", AU_UNIT_VW},
    {"vh", AU_UNIT_VH},     {"vi", AU_UNIT_VI},       {"vb", AU_UNIT_VB},       {"vmin", AU_UNIT_VMIN},
    {"vmax", AU_UNIT_VMAX}, {"svw", AU_UNIT_SVW},     {"svh", AU_UNIT_SVH},     {"svi", AU_UNIT_SVI},
    {"svb", AU_UNIT_SVB},   {"svmin", AU_UNIT_SVMIN}, {"svmax", AU_UNIT_SVMAX}, {"lvw", AU_UNIT_LVW},
    {"lvh", AU_UNIT_LVH},   {"lvi", AU_UNIT_LVI},     {"lvb", AU_UNIT_LVB},     {"lvmin", AU_UNIT_LVMIN},
    {"lvmax", AU_UNIT_LVMAX}, {"dvw", AU_UNIT_DVW},   {"dvh", AU_UNIT_DVH},     {"dvi", AU_UNIT_DVI},
    {"dvb", AU_UNIT_DVB},   {"dvmin", AU_UNIT_DVMIN}, {"dvmax", AU_UNIT_DVMAX}, {"cqw", AU_UNIT_CQW},
    {"cqh", AU_UNIT_CQH},   {"cqi", AU_UNIT_CQI},     {"cqb", AU_UNIT_CQB},     {"cqmin", AU_UNIT_CQMIN},
    {"cqmax", AU_UNIT_CQMAX}, {"deg", AU_UNIT_DEG},   {"grad", AU_UNIT_GRAD},   {"rad", AU_UNIT_RAD},
    {"turn", AU_UNIT_TURN}, {"s", AU_UNIT_S},         {"ms", AU_UNIT_MS},       {"hz", AU_UNIT_HZ},
    {"khz", AU_UNIT_KHZ},   {"dpi", AU_UNIT_DPI},     {"dpcm", AU_UNIT_DPCM},   {"dppx", AU_UNIT_DPPX},
    {"x", AU_UNIT_X},       {"fr", AU_UNIT_FR},
};

const char *css_unit_name(int unit)
{
    for (size_t i = 0; i < CSS_ARRAY_LEN(unit_names); i++)
        if (unit_names[i].unit == unit)
            return unit_names[i].name;
    return "";
}

static int unit_lookup(const char *s, size_t n)
{
    char buf[8];
    if (!css_lower(s, n, buf, sizeof buf))
        return AU_UNIT_UNKNOWN;
    for (size_t i = 0; i < CSS_ARRAY_LEN(unit_names); i++)
        if (strcmp(unit_names[i].name, buf) == 0)
            return unit_names[i].unit;
    return AU_UNIT_UNKNOWN;
}

/* ---- Tokenizer ---- */

typedef struct {
    const unsigned char *s;
    size_t n, i;
    CssTokens *out;
} Lexer;

static int peek(const Lexer *L, size_t k)
{
    size_t j = L->i + k;
    return j < L->n ? L->s[j] : -1;
}

static bool is_newline(int c) { return c == '\n' || c == '\r' || c == '\f'; }
static bool is_ws(int c) { return c == ' ' || c == '\t' || is_newline(c); }
static bool is_ident_start(int c) { return au_is_alpha(c) || c == '_' || c >= 0x80; }
static bool is_ident_char(int c) { return is_ident_start(c) || au_is_digit(c) || c == '-'; }

static bool valid_escape(int c1, int c2) { return c1 == '\\' && c2 != -1 && !is_newline(c2); }

static bool starts_ident(int c1, int c2, int c3)
{
    if (c1 == '-')
        return is_ident_start(c2) || c2 == '-' || valid_escape(c2, c3);
    if (is_ident_start(c1))
        return true;
    return valid_escape(c1, c2);
}

static bool starts_number(int c1, int c2, int c3)
{
    if (c1 == '+' || c1 == '-') {
        if (au_is_digit(c2))
            return true;
        return c2 == '.' && au_is_digit(c3);
    }
    if (c1 == '.')
        return au_is_digit(c2);
    return au_is_digit(c1);
}

/* Appends the code point at L->i (decoding UTF-8) to the pool. */
static void copy_cp(Lexer *L)
{
    unsigned char c = L->s[L->i];
    if (c < 0x80) {
        au_buf_putc(&L->out->pool, c ? (char)c : 0);
        if (!c) { /* NUL becomes U+FFFD */
            L->out->pool.len--;
            au_buf_put_utf8(&L->out->pool, 0xFFFD);
        }
        L->i++;
        return;
    }
    size_t adv;
    uint32_t cp = au_utf8_decode((const char *)L->s + L->i, L->n - L->i, &adv);
    au_buf_put_utf8(&L->out->pool, cp);
    L->i += adv;
}

/* Consumes an escape; L->i is just after the backslash. */
static void consume_escape(Lexer *L)
{
    int c = peek(L, 0);
    if (c == -1) {
        au_buf_put_utf8(&L->out->pool, 0xFFFD);
        return;
    }
    if (au_is_hex(c)) {
        uint32_t v = 0;
        int k = 0;
        while (k < 6 && au_is_hex(peek(L, 0))) {
            int h = peek(L, 0);
            v = v * 16 + (uint32_t)(au_is_digit(h) ? h - '0' : au_lower(h) - 'a' + 10);
            L->i++;
            k++;
        }
        int w = peek(L, 0);
        if (w == '\r' && peek(L, 1) == '\n')
            L->i += 2;
        else if (is_ws(w))
            L->i++;
        if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
            v = 0xFFFD;
        au_buf_put_utf8(&L->out->pool, v);
        return;
    }
    copy_cp(L);
}

/* Consumes an ident sequence into the pool (not terminated). */
static void consume_ident_seq(Lexer *L)
{
    for (;;) {
        int c = peek(L, 0);
        if (c != -1 && is_ident_char(c)) {
            copy_cp(L);
        } else if (valid_escape(c, peek(L, 1))) {
            L->i++;
            consume_escape(L);
        } else {
            return;
        }
    }
}

static CssToken *push_tok(Lexer *L, int type)
{
    CssTokens *o = L->out;
    CssToken *t = CSS_PUSH(o->t, o->n, o->cap);
    memset(t, 0, sizeof *t);
    t->type = (uint8_t)type;
    return t;
}

static void begin_str(Lexer *L, CssToken *t) { t->str = (uint32_t)L->out->pool.len; }
static void end_str(Lexer *L, CssToken *t)
{
    t->len = (uint32_t)(L->out->pool.len - t->str);
    au_buf_putc(&L->out->pool, 0);
}

static void set_kw(Lexer *L, CssToken *t)
{
    t->kw = css_kw_lookup(L->out->pool.data + t->str, t->len);
}

static double pow10i(int e)
{
    double r = 1, b = 10;
    bool neg = e < 0;
    unsigned u = (unsigned)(neg ? -e : e);
    if (u > 400)
        u = 400;
    while (u) {
        if (u & 1)
            r *= b;
        b *= b;
        u >>= 1;
    }
    return neg ? 1 / r : r;
}

/* Consumes a number; returns its value and sets *integer. */
static double consume_number(Lexer *L, bool *integer, bool *sign)
{
    double sgn = 1;
    *integer = true;
    *sign = false;
    int c = peek(L, 0);
    if (c == '+' || c == '-') {
        if (c == '-')
            sgn = -1;
        *sign = true;
        L->i++;
    }
    double v = 0;
    while (au_is_digit(peek(L, 0))) {
        v = v * 10 + (peek(L, 0) - '0');
        L->i++;
    }
    int frac = 0;
    if (peek(L, 0) == '.' && au_is_digit(peek(L, 1))) {
        *integer = false;
        L->i++;
        while (au_is_digit(peek(L, 0))) {
            v = v * 10 + (peek(L, 0) - '0');
            frac++;
            L->i++;
        }
    }
    int e = 0;
    c = peek(L, 0);
    if (c == 'e' || c == 'E') {
        int c1 = peek(L, 1), c2 = peek(L, 2);
        if (au_is_digit(c1) || ((c1 == '+' || c1 == '-') && au_is_digit(c2))) {
            *integer = false;
            L->i++;
            int es = 1;
            if (c1 == '+' || c1 == '-') {
                es = c1 == '-' ? -1 : 1;
                L->i++;
            }
            while (au_is_digit(peek(L, 0))) {
                if (e < 10000)
                    e = e * 10 + (peek(L, 0) - '0');
                L->i++;
            }
            e *= es;
        }
    }
    return sgn * v * pow10i(e - frac);
}

static void consume_numeric(Lexer *L)
{
    bool integer, sign;
    double v = consume_number(L, &integer, &sign);
    float f = (float)v;
    if (v > 3.0e38)
        f = 3.0e38f;
    else if (v < -3.0e38)
        f = -3.0e38f;
    if (starts_ident(peek(L, 0), peek(L, 1), peek(L, 2))) {
        CssToken *t = push_tok(L, TK_DIMENSION);
        t->num = f;
        t->flags = (uint8_t)((integer ? TF_INTEGER : 0) | (sign ? TF_SIGN : 0));
        begin_str(L, t);
        consume_ident_seq(L);
        end_str(L, t);
        t->unit = (uint16_t)unit_lookup(L->out->pool.data + t->str, t->len);
    } else if (peek(L, 0) == '%') {
        L->i++;
        CssToken *t = push_tok(L, TK_PERCENT);
        t->num = f;
        t->flags = sign ? TF_SIGN : 0;
    } else {
        CssToken *t = push_tok(L, TK_NUMBER);
        t->num = f;
        t->flags = (uint8_t)((integer ? TF_INTEGER : 0) | (sign ? TF_SIGN : 0));
    }
}

static void consume_bad_url_rest(Lexer *L)
{
    for (;;) {
        int c = peek(L, 0);
        if (c == -1)
            return;
        if (c == ')') {
            L->i++;
            return;
        }
        if (valid_escape(c, peek(L, 1))) {
            L->i++;
            consume_escape(L);
            continue;
        }
        L->i++;
    }
}

static void consume_url(Lexer *L)
{
    while (is_ws(peek(L, 0)))
        L->i++;
    CssToken *t = push_tok(L, TK_URL);
    size_t ti = L->out->n - 1;
    begin_str(L, t);
    for (;;) {
        int c = peek(L, 0);
        if (c == ')') {
            L->i++;
            break;
        }
        if (c == -1)
            break;
        if (is_ws(c)) {
            while (is_ws(peek(L, 0)))
                L->i++;
            if (peek(L, 0) == ')') {
                L->i++;
                break;
            }
            if (peek(L, 0) == -1)
                break;
            consume_bad_url_rest(L);
            L->out->t[ti].type = TK_BAD_URL;
            break;
        }
        if (c == '"' || c == '\'' || c == '(' || c == 0x7F || (c >= 0 && c <= 8) || c == 0x0B ||
            (c >= 0x0E && c <= 0x1F)) {
            consume_bad_url_rest(L);
            L->out->t[ti].type = TK_BAD_URL;
            break;
        }
        if (c == '\\') {
            if (valid_escape(c, peek(L, 1))) {
                L->i++;
                consume_escape(L);
                continue;
            }
            consume_bad_url_rest(L);
            L->out->t[ti].type = TK_BAD_URL;
            break;
        }
        copy_cp(L);
    }
    end_str(L, &L->out->t[ti]);
}

static void consume_ident_like(Lexer *L)
{
    CssToken *t = push_tok(L, TK_IDENT);
    size_t ti = L->out->n - 1;
    begin_str(L, t);
    consume_ident_seq(L);
    end_str(L, &L->out->t[ti]);
    t = &L->out->t[ti];
    const char *name = L->out->pool.data + t->str;
    if (t->len == 3 && au_ieq(name, 3, "url", 3) && peek(L, 0) == '(') {
        L->i++;
        size_t j = L->i;
        while (j < L->n && is_ws(L->s[j]))
            j++;
        if (j < L->n && (L->s[j] == '"' || L->s[j] == '\'')) {
            t->type = TK_FUNCTION;
            set_kw(L, t);
            return;
        }
        /* a url token replaces the ident */
        L->out->n--;
        L->out->pool.len = t->str;
        consume_url(L);
        return;
    }
    if (peek(L, 0) == '(') {
        L->i++;
        t->type = TK_FUNCTION;
    }
    set_kw(L, t);
}

static void consume_string(Lexer *L, int quote)
{
    CssToken *t = push_tok(L, TK_STRING);
    size_t ti = L->out->n - 1;
    begin_str(L, t);
    for (;;) {
        int c = peek(L, 0);
        if (c == -1 || c == quote) {
            if (c == quote)
                L->i++;
            break;
        }
        if (is_newline(c)) {
            L->out->t[ti].type = TK_BAD_STRING;
            break;
        }
        if (c == '\\') {
            int c2 = peek(L, 1);
            if (c2 == -1) {
                L->i++;
                continue;
            }
            if (is_newline(c2)) {
                L->i += 2;
                if (c2 == '\r' && peek(L, 0) == '\n')
                    L->i++;
                continue;
            }
            L->i++;
            consume_escape(L);
            continue;
        }
        copy_cp(L);
    }
    end_str(L, &L->out->t[ti]);
}

bool css_tok_is_close(int type) { return type == TK_RPAREN || type == TK_RBRACKET || type == TK_RBRACE; }

static int close_of(int type)
{
    switch (type) {
    case TK_FUNCTION:
    case TK_LPAREN:
        return TK_RPAREN;
    case TK_LBRACKET:
        return TK_RBRACKET;
    case TK_LBRACE:
        return TK_RBRACE;
    default:
        return 0;
    }
}

/* Sets len of every opening token to the index after its matching close. */
static void match_blocks(CssTokens *tk, uint32_t from)
{
    uint32_t *stack = NULL, sn = 0, scap = 0;
    for (uint32_t i = from; i < tk->n; i++) {
        int ty = tk->t[i].type;
        if (close_of(ty)) {
            *CSS_PUSH(stack, sn, scap) = i;
        } else if (css_tok_is_close(ty) && sn && close_of(tk->t[stack[sn - 1]].type) == ty) {
            tk->t[stack[--sn]].len = i + 1;
        }
    }
    while (sn)
        tk->t[stack[--sn]].len = tk->n;
    au_free(stack);
}

void css_tokens_rematch(CssTokens *tk) { match_blocks(tk, 0); }

void css_tokenize(const char *src, size_t len, CssTokens *out)
{
    Lexer L = {(const unsigned char *)src, len, 0, out};
    uint32_t first = out->n;
    if (!out->pool.data)
        au_buf_putc(&out->pool, 0); /* offset 0 is the empty string */
    while (L.i < L.n) {
        int c = peek(&L, 0);
        if (c == '/' && peek(&L, 1) == '*') {
            L.i += 2;
            while (L.i < L.n && !(L.s[L.i] == '*' && peek(&L, 1) == '/'))
                L.i++;
            L.i = L.i + 2 > L.n ? L.n : L.i + 2;
            continue;
        }
        if (is_ws(c)) {
            while (is_ws(peek(&L, 0)))
                L.i++;
            push_tok(&L, TK_WS);
            continue;
        }
        switch (c) {
        case '"':
        case '\'':
            L.i++;
            consume_string(&L, c);
            continue;
        case '#':
            if (is_ident_char(peek(&L, 1)) || valid_escape(peek(&L, 1), peek(&L, 2))) {
                L.i++;
                CssToken *t = push_tok(&L, TK_HASH);
                size_t ti = out->n - 1;
                if (starts_ident(peek(&L, 0), peek(&L, 1), peek(&L, 2)))
                    t->flags = TF_ID;
                begin_str(&L, t);
                consume_ident_seq(&L);
                end_str(&L, &out->t[ti]);
            } else {
                L.i++;
                push_tok(&L, TK_DELIM)->num = '#';
            }
            continue;
        case '(':
            L.i++;
            push_tok(&L, TK_LPAREN);
            continue;
        case ')':
            L.i++;
            push_tok(&L, TK_RPAREN);
            continue;
        case '[':
            L.i++;
            push_tok(&L, TK_LBRACKET);
            continue;
        case ']':
            L.i++;
            push_tok(&L, TK_RBRACKET);
            continue;
        case '{':
            L.i++;
            push_tok(&L, TK_LBRACE);
            continue;
        case '}':
            L.i++;
            push_tok(&L, TK_RBRACE);
            continue;
        case ',':
            L.i++;
            push_tok(&L, TK_COMMA);
            continue;
        case ':':
            L.i++;
            push_tok(&L, TK_COLON);
            continue;
        case ';':
            L.i++;
            push_tok(&L, TK_SEMI);
            continue;
        case '+':
            if (starts_number(c, peek(&L, 1), peek(&L, 2))) {
                consume_numeric(&L);
            } else {
                L.i++;
                push_tok(&L, TK_DELIM)->num = '+';
            }
            continue;
        case '-':
            if (starts_number(c, peek(&L, 1), peek(&L, 2))) {
                consume_numeric(&L);
            } else if (peek(&L, 1) == '-' && peek(&L, 2) == '>') {
                L.i += 3;
                push_tok(&L, TK_CDC);
            } else if (starts_ident(c, peek(&L, 1), peek(&L, 2))) {
                consume_ident_like(&L);
            } else {
                L.i++;
                push_tok(&L, TK_DELIM)->num = '-';
            }
            continue;
        case '.':
            if (starts_number(c, peek(&L, 1), peek(&L, 2))) {
                consume_numeric(&L);
            } else {
                L.i++;
                push_tok(&L, TK_DELIM)->num = '.';
            }
            continue;
        case '<':
            if (peek(&L, 1) == '!' && peek(&L, 2) == '-' && peek(&L, 3) == '-') {
                L.i += 4;
                push_tok(&L, TK_CDO);
            } else {
                L.i++;
                push_tok(&L, TK_DELIM)->num = '<';
            }
            continue;
        case '@':
            if (starts_ident(peek(&L, 1), peek(&L, 2), peek(&L, 3))) {
                L.i++;
                CssToken *t = push_tok(&L, TK_AT);
                size_t ti = out->n - 1;
                begin_str(&L, t);
                consume_ident_seq(&L);
                end_str(&L, &out->t[ti]);
                set_kw(&L, &out->t[ti]);
            } else {
                L.i++;
                push_tok(&L, TK_DELIM)->num = '@';
            }
            continue;
        case '\\':
            if (valid_escape(c, peek(&L, 1))) {
                consume_ident_like(&L);
            } else {
                L.i++;
                push_tok(&L, TK_DELIM)->num = '\\';
            }
            continue;
        default:
            break;
        }
        if (au_is_digit(c)) {
            consume_numeric(&L);
        } else if (is_ident_start(c) || c == 0) {
            if (c == 0) {
                /* U+0000 is an ident code point after replacement by U+FFFD */
                consume_ident_like(&L);
            } else {
                consume_ident_like(&L);
            }
        } else {
            size_t adv = 1;
            uint32_t cp = c < 0x80 ? (uint32_t)c : au_utf8_decode(src + L.i, L.n - L.i, &adv);
            L.i += adv;
            push_tok(&L, TK_DELIM)->num = (float)cp;
        }
    }
    match_blocks(out, first);
}

void css_tokens_free(CssTokens *tk)
{
    au_free(tk->t);
    au_buf_free(&tk->pool);
    tk->t = NULL;
    tk->n = tk->cap = 0;
}

/* ---- Token lists ---- */

void css_tokens_append(CssTokens *dst, const CssTokens *src, uint32_t a, uint32_t b)
{
    if (!dst->pool.data)
        au_buf_putc(&dst->pool, 0);
    uint32_t base = dst->n;
    for (uint32_t i = a; i < b; i++) {
        CssToken t = src->t[i];
        switch (t.type) {
        case TK_IDENT:
        case TK_FUNCTION:
        case TK_AT:
        case TK_HASH:
        case TK_STRING:
        case TK_BAD_STRING:
        case TK_URL:
        case TK_BAD_URL:
        case TK_DIMENSION: {
            uint32_t off = (uint32_t)dst->pool.len;
            au_buf_append(&dst->pool, src->pool.data + t.str, t.len);
            au_buf_putc(&dst->pool, 0);
            t.str = off;
            break;
        }
        default:
            t.str = 0;
            break;
        }
        if (close_of(t.type)) {
            uint32_t end = t.len > b ? b : t.len;
            t.len = end - a + base;
        }
        *CSS_PUSH(dst->t, dst->n, dst->cap) = t;
    }
}

CssTokList *css_toklist_new(const CssTokens *src, uint32_t a, uint32_t b)
{
    CssTokList *l = au_calloc(sizeof *l);
    l->refs = 1;
    css_tokens_append(&l->tk, src, a, b);
    for (uint32_t i = 0; i < l->tk.n; i++) {
        const CssToken *t = &l->tk.t[i];
        if (t->type == TK_FUNCTION && (t->kw == AU_K_var || t->kw == AU_K_env))
            l->has_var = 1;
        if (t->type == TK_FUNCTION && t->kw == AU_K_attr)
            l->has_attr = 1;
    }
    return l;
}

CssTokList *css_toklist_retain(CssTokList *l)
{
    if (l)
        l->refs++;
    return l;
}

void css_toklist_release(CssTokList *l)
{
    if (!l || --l->refs)
        return;
    css_tokens_free(&l->tk);
    au_free(l);
}

/* ---- Serialization (the primitives are in serialize.c) ---- */

static bool word_like_end(const CssToken *t)
{
    switch (t->type) {
    case TK_IDENT:
    case TK_AT:
    case TK_HASH:
    case TK_DIMENSION:
    case TK_NUMBER:
    case TK_PERCENT:
    case TK_URL:
    case TK_BAD_URL:
    case TK_CDC:
        return true;
    case TK_DELIM:
        return t->num == '#' || t->num == '-' || t->num == '@' || t->num == '.' || t->num == '+';
    default:
        return false;
    }
}

static bool word_like_start(const CssToken *t)
{
    switch (t->type) {
    case TK_IDENT:
    case TK_FUNCTION:
    case TK_URL:
    case TK_BAD_URL:
    case TK_NUMBER:
    case TK_PERCENT:
    case TK_DIMENSION:
    case TK_CDC:
    case TK_LPAREN:
        return true;
    case TK_DELIM:
        return t->num == '-' || t->num == '%';
    default:
        return false;
    }
}

void css_tokens_serialize(const CssTokens *tk, uint32_t a, uint32_t b, AuBuf *out)
{
    const CssToken *prev = NULL;
    for (uint32_t i = a; i < b; i++) {
        const CssToken *t = &tk->t[i];
        const char *s = tk->pool.data + t->str;
        if (prev && word_like_end(prev) && word_like_start(t)) {
            /* an ident followed by ( would become a function; numbers merge */
            if (!(t->type == TK_LPAREN && prev->type != TK_IDENT))
                au_buf_puts(out, "/**/");
        }
        switch (t->type) {
        case TK_IDENT:
            css_serialize_ident(s, t->len, out);
            break;
        case TK_FUNCTION:
            css_serialize_ident(s, t->len, out);
            au_buf_putc(out, '(');
            break;
        case TK_AT:
            au_buf_putc(out, '@');
            css_serialize_ident(s, t->len, out);
            break;
        case TK_HASH:
            au_buf_putc(out, '#');
            css_serialize_ident(s, t->len, out);
            break;
        case TK_STRING:
            css_serialize_string(s, t->len, out);
            break;
        case TK_BAD_STRING:
            au_buf_putc(out, '"');
            break;
        case TK_URL:
            au_buf_puts(out, "url(");
            css_serialize_string(s, t->len, out);
            au_buf_putc(out, ')');
            break;
        case TK_BAD_URL:
            au_buf_puts(out, "url()");
            break;
        case TK_DELIM:
            if (t->num == '\\')
                au_buf_puts(out, "\\\n");
            else
                au_buf_put_utf8(out, (uint32_t)t->num);
            break;
        case TK_NUMBER:
            if ((t->flags & TF_SIGN) && t->num >= 0)
                au_buf_putc(out, '+');
            css_serialize_number(t->num, out);
            break;
        case TK_PERCENT:
            if ((t->flags & TF_SIGN) && t->num >= 0)
                au_buf_putc(out, '+');
            css_serialize_number(t->num, out);
            au_buf_putc(out, '%');
            break;
        case TK_DIMENSION:
            if ((t->flags & TF_SIGN) && t->num >= 0)
                au_buf_putc(out, '+');
            css_serialize_number(t->num, out);
            /* a unit starting with e followed by a digit would read as an exponent */
            if (t->len && (s[0] == 'e' || s[0] == 'E') && t->len > 1 && au_is_digit((unsigned char)s[1])) {
                au_buf_printf(out, "\\%x ", (unsigned char)s[0]);
                css_serialize_ident(s + 1, t->len - 1, out);
            } else {
                css_serialize_ident(s, t->len, out);
            }
            break;
        case TK_WS:
            au_buf_putc(out, ' ');
            break;
        case TK_CDO:
            au_buf_puts(out, "<!--");
            break;
        case TK_CDC:
            au_buf_puts(out, "-->");
            break;
        case TK_COLON:
            au_buf_putc(out, ':');
            break;
        case TK_SEMI:
            au_buf_putc(out, ';');
            break;
        case TK_COMMA:
            au_buf_putc(out, ',');
            break;
        case TK_LBRACKET:
            au_buf_putc(out, '[');
            break;
        case TK_RBRACKET:
            au_buf_putc(out, ']');
            break;
        case TK_LPAREN:
            au_buf_putc(out, '(');
            break;
        case TK_RPAREN:
            au_buf_putc(out, ')');
            break;
        case TK_LBRACE:
            au_buf_putc(out, '{');
            break;
        case TK_RBRACE:
            au_buf_putc(out, '}');
            break;
        default:
            break;
        }
        prev = t;
    }
}
