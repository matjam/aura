/* HTML tokenizer: the state machine of HTML 13.2.5.
 *
 * Input is newline-normalized, valid UTF-8 in t->buf. Each state looks at
 * the character at t->pos (peek) and moves past it only when it consumes
 * it, so "reconsume in state X" is a state change without advancing. When
 * the available input runs out, html_tok_run returns and resumes in the
 * same state once more input arrives; states that need to look ahead
 * (markup declarations, DOCTYPE keywords, named character references)
 * wait until enough input is present or the input is complete.
 *
 * Characters are collected in t->chars and emitted as one character token
 * before any other token and whenever the tokenizer stops. */
#include "html_internal.h"

#include <string.h>

#define C_NEED (-2)
#define C_EOF (-1)

enum {
    S_DATA = HTML_STATE_DATA,
    S_RCDATA = HTML_STATE_RCDATA,
    S_RAWTEXT = HTML_STATE_RAWTEXT,
    S_SCRIPT = HTML_STATE_SCRIPT_DATA,
    S_PLAINTEXT = HTML_STATE_PLAINTEXT,
    S_CDATA = HTML_STATE_CDATA_SECTION,
    S_TAG_OPEN = HTML_STATE_COUNT_PUBLIC,
    S_END_TAG_OPEN,
    S_TAG_NAME,
    S_TEXT_LT,          /* RCDATA or RAWTEXT less-than sign (text_state) */
    S_TEXT_END_OPEN,    /* RCDATA, RAWTEXT, script data (escaped) end tag open */
    S_TEXT_END_NAME,
    S_SCRIPT_LT,
    S_SCRIPT_ESC_START,
    S_SCRIPT_ESC_START_DASH,
    S_SCRIPT_ESC,
    S_SCRIPT_ESC_DASH,
    S_SCRIPT_ESC_DASH_DASH,
    S_SCRIPT_ESC_LT,
    S_SCRIPT_DESC_START,
    S_SCRIPT_DESC,
    S_SCRIPT_DESC_DASH,
    S_SCRIPT_DESC_DASH_DASH,
    S_SCRIPT_DESC_LT,
    S_SCRIPT_DESC_END,
    S_BEFORE_ATTR_NAME,
    S_ATTR_NAME,
    S_AFTER_ATTR_NAME,
    S_BEFORE_ATTR_VALUE,
    S_ATTR_VALUE_DQ,
    S_ATTR_VALUE_SQ,
    S_ATTR_VALUE_UQ,
    S_AFTER_ATTR_VALUE_Q,
    S_SELF_CLOSING,
    S_BOGUS_COMMENT,
    S_MARKUP_DECL,
    S_COMMENT_START,
    S_COMMENT_START_DASH,
    S_COMMENT,
    S_COMMENT_LT,
    S_COMMENT_LT_BANG,
    S_COMMENT_LT_BANG_DASH,
    S_COMMENT_LT_BANG_DASH_DASH,
    S_COMMENT_END_DASH,
    S_COMMENT_END,
    S_COMMENT_END_BANG,
    S_DOCTYPE,
    S_BEFORE_DOCTYPE_NAME,
    S_DOCTYPE_NAME,
    S_AFTER_DOCTYPE_NAME,
    S_AFTER_DOCTYPE_PUBLIC_KW,
    S_BEFORE_DOCTYPE_PUBLIC_ID,
    S_DOCTYPE_PUBLIC_ID_DQ,
    S_DOCTYPE_PUBLIC_ID_SQ,
    S_AFTER_DOCTYPE_PUBLIC_ID,
    S_BETWEEN_DOCTYPE_IDS,
    S_AFTER_DOCTYPE_SYSTEM_KW,
    S_BEFORE_DOCTYPE_SYSTEM_ID,
    S_DOCTYPE_SYSTEM_ID_DQ,
    S_DOCTYPE_SYSTEM_ID_SQ,
    S_AFTER_DOCTYPE_SYSTEM_ID,
    S_BOGUS_DOCTYPE,
    S_CDATA_BRACKET,
    S_CDATA_END,
    S_PI_OPEN,
    S_PI_TARGET,
    S_AFTER_PI_TARGET,
    S_PI_DATA,
    S_PI_QUESTION,
    S_CHARREF,
    S_NAMED_CHARREF,
    S_AMBIGUOUS_AMP,
    S_NUMERIC_CHARREF,
    S_HEX_CHARREF_START,
    S_HEX_CHARREF,
    S_DEC_CHARREF,
    S_NUMERIC_CHARREF_END
};

/* Character classes for the bulk scanner: a byte stops a run if its class
 * shares a bit with the state's mask. K_ALWAYS bytes always stop it. */
enum {
    K_ALWAYS = 1,
    K_AMP = 2,
    K_LT = 4,
    K_DQ = 8,
    K_SQ = 16,
    K_DASH = 32,
    K_UQ = 64,  /* ends an unquoted attribute value or needs an error there */
    K_RB = 128, /* ']' */
    K_GT = 256
};

static const uint16_t ch_class[128] = {
    [0x00] = K_ALWAYS, [0x01] = K_ALWAYS, [0x02] = K_ALWAYS, [0x03] = K_ALWAYS,
    [0x04] = K_ALWAYS, [0x05] = K_ALWAYS, [0x06] = K_ALWAYS, [0x07] = K_ALWAYS,
    [0x08] = K_ALWAYS, ['\t'] = K_UQ, ['\n'] = K_UQ, [0x0B] = K_ALWAYS,
    ['\f'] = K_UQ, [0x0D] = K_ALWAYS, [0x0E] = K_ALWAYS, [0x0F] = K_ALWAYS,
    [0x10] = K_ALWAYS, [0x11] = K_ALWAYS, [0x12] = K_ALWAYS, [0x13] = K_ALWAYS,
    [0x14] = K_ALWAYS, [0x15] = K_ALWAYS, [0x16] = K_ALWAYS, [0x17] = K_ALWAYS,
    [0x18] = K_ALWAYS, [0x19] = K_ALWAYS, [0x1A] = K_ALWAYS, [0x1B] = K_ALWAYS,
    [0x1C] = K_ALWAYS, [0x1D] = K_ALWAYS, [0x1E] = K_ALWAYS, [0x1F] = K_ALWAYS,
    [' '] = K_UQ, ['"'] = K_DQ | K_UQ, ['&'] = K_AMP, ['\''] = K_SQ | K_UQ,
    ['-'] = K_DASH, ['<'] = K_LT | K_UQ, ['='] = K_UQ, ['>'] = K_UQ | K_GT,
    [']'] = K_RB, ['`'] = K_UQ, [0x7F] = K_ALWAYS,
};

/* ---- Setup ---- */

void html_tok_init(HtmlTokenizer *t)
{
    memset(t, 0, sizeof *t);
    t->ins = (size_t)-1;
    t->line = 1;
    t->col = 1;
    t->state = S_DATA;
}

void html_tok_free(HtmlTokenizer *t)
{
    au_free(t->buf);
    au_buf_free(&t->name);
    au_buf_free(&t->attrname);
    au_buf_free(&t->vals);
    au_buf_free(&t->data);
    au_buf_free(&t->pub);
    au_buf_free(&t->sys);
    au_buf_free(&t->tmp);
    au_buf_free(&t->chars);
    au_free(t->attrs);
    au_free(t->attr_set);
}

static void reserve_input(HtmlTokenizer *t, size_t extra)
{
    /* Drop consumed input once it is large compared with what is left.
     * Not while an insertion point is defined: the parser keeps saved
     * insertion points as offsets too. */
    if (t->pos >= 4096 && t->pos >= t->len / 2 && t->ins == (size_t)-1) {
        size_t drop = t->pos;
        memmove(t->buf, t->buf + drop, t->len - drop);
        t->len -= drop;
        t->pos = 0;
        t->checked = t->checked > drop ? t->checked - drop : 0;
    }
    if (t->len + extra > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->len + extra)
            cap *= 2;
        t->buf = au_realloc(t->buf, cap);
        t->cap = cap;
    }
}

void html_tok_append(HtmlTokenizer *t, const char *s, size_t n)
{
    if (!n)
        return;
    reserve_input(t, n);
    memcpy(t->buf + t->len, s, n);
    t->len += n;
}

void html_tok_insert(HtmlTokenizer *t, size_t at, const char *s, size_t n)
{
    if (!n)
        return;
    size_t off = at - t->pos; /* reserve_input may move the buffer */
    reserve_input(t, n);
    at = t->pos + off;
    memmove(t->buf + at + n, t->buf + at, t->len - at);
    memcpy(t->buf + at, s, n);
    t->len += n;
    if (t->checked > at)
        t->checked += n;
}

void html_tok_set_state(HtmlTokenizer *t, int state)
{
    t->state = (uint8_t)state;
}

void html_tok_set_last_start_tag(HtmlTokenizer *t, AuAtom tag)
{
    t->last_start_tag = tag;
}

/* ---- Errors and input ---- */

static void err(HtmlTokenizer *t, const char *code)
{
    if (t->error)
        t->error(t->error_ctx, code, t->line, t->col);
}

/* End of file inside a processing instruction's data. The html5lib tests
 * place this error where the processing instruction token was created
 * (just after the target), so report it there. */
static void err_pi_eof(HtmlTokenizer *t)
{
    if (t->error)
        t->error(t->error_ctx, "eof-in-processing-instruction", t->pi_line, t->pi_col);
}

/* Reports an error at the character just consumed (an ASCII character on
 * the current line). */
static void err_prev(HtmlTokenizer *t, const char *code)
{
    if (t->error)
        t->error(t->error_ctx, code, t->line, t->col - 1);
}

static void check_input_char(HtmlTokenizer *t, int c)
{
    if ((c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE)
        err(t, "noncharacter-in-input-stream");
    else if ((c >= 0x01 && c <= 0x1F && c != '\t' && c != '\n' && c != '\f' && c != '\r') ||
             (c >= 0x7F && c <= 0x9F))
        err(t, "control-character-in-input-stream");
}

static size_t avail_end(const HtmlTokenizer *t)
{
    return (t->limit_ins && t->ins < t->len) ? t->ins : t->len;
}

/* True if no input beyond avail_end will ever arrive. */
static bool input_final(const HtmlTokenizer *t)
{
    return t->eof && !t->limit_ins;
}

static int peek(HtmlTokenizer *t, size_t *n)
{
    size_t end = avail_end(t);
    if (t->pos >= end)
        return (t->pos >= t->len && input_final(t)) ? C_EOF : C_NEED;
    const unsigned char *u = (const unsigned char *)t->buf + t->pos;
    int c;
    if (u[0] < 0x80) {
        *n = 1;
        c = u[0];
    } else if (u[0] < 0xE0) {
        *n = 2;
        c = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    } else if (u[0] < 0xF0) {
        *n = 3;
        c = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    } else {
        *n = 4;
        c = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    }
    if (t->pos >= t->checked) {
        t->checked = t->pos + *n;
        if (c < 0x20 || c >= 0x7F)
            check_input_char(t, c);
    }
    return c;
}

/* Columns count UTF-16 code units, as the html5lib tests do. */
static void adv(HtmlTokenizer *t, int c, size_t n)
{
    t->pos += n;
    if (c == '\n') {
        t->line++;
        t->col = 1;
    } else {
        t->col += c >= 0x10000 ? 2 : 1;
    }
}

/* Moves past a run of plain ASCII bytes (none of whose classes meet mask),
 * appending them to dst. */
static void scan(HtmlTokenizer *t, AuBuf *dst, unsigned mask)
{
    size_t end = avail_end(t);
    size_t i = t->pos;
    const unsigned char *u = (const unsigned char *)t->buf;
    mask |= K_ALWAYS;
    while (i < end && u[i] < 0x80 && !(ch_class[u[i]] & mask)) {
        if (u[i] == '\n') {
            t->line++;
            t->col = 1;
        } else {
            t->col++;
        }
        i++;
    }
    if (i > t->pos) {
        if (dst)
            au_buf_append(dst, t->buf + t->pos, i - t->pos);
        t->pos = i;
        if (t->checked < i)
            t->checked = i;
    }
}

/* Moves past n ASCII characters already examined by lookahead. */
static void skip_ascii(HtmlTokenizer *t, size_t n)
{
    for (size_t i = 0; i < n; i++)
        adv(t, (unsigned char)t->buf[t->pos], 1);
    if (t->checked < t->pos)
        t->checked = t->pos;
}

/* Lookahead for a case-insensitive (or exact) keyword at pos. Returns 1 on
 * a match, 0 on a mismatch, C_NEED if more input could decide it. */
static int lookahead(HtmlTokenizer *t, const char *kw, bool icase)
{
    size_t n = strlen(kw), end = avail_end(t);
    for (size_t i = 0; i < n; i++) {
        if (t->pos + i >= end)
            return input_final(t) ? 0 : C_NEED;
        int c = (unsigned char)t->buf[t->pos + i];
        if (icase)
            c = au_lower(c);
        if (c != kw[i])
            return 0;
    }
    return 1;
}

/* ---- Emitting ---- */

static void put_cp(AuBuf *b, int c)
{
    if (c < 0x80)
        au_buf_putc(b, (char)c);
    else
        au_buf_put_utf8(b, (uint32_t)c);
}

static void emit_char(HtmlTokenizer *t, int c)
{
    put_cp(&t->chars, c);
}

static void flush_chars(HtmlTokenizer *t)
{
    if (!t->chars.len)
        return;
    HtmlToken tok;
    memset(&tok, 0, sizeof tok);
    tok.type = HTML_TOK_CHARS;
    tok.data = t->chars.data;
    tok.len = t->chars.len;
    t->chars.len = 0;
    t->emit(t->sink, &tok);
}

static void set_clear(HtmlTokenizer *t);

static void new_tag(HtmlTokenizer *t, bool end)
{
    set_clear(t); /* a tag abandoned at end of input may have left names */
    t->end_tag = end;
    t->self_closing = false;
    t->name.len = 0;
    t->vals.len = 0;
    t->nattrs = 0;
    t->attr_live = false;
}

/* Records the length of the current attribute's value. */
static void close_attr(HtmlTokenizer *t)
{
    if (t->attr_live) {
        HtmlAttr *a = &t->attrs[t->nattrs - 1];
        a->vlen = (uint32_t)(t->vals.len - a->voff);
        t->attr_live = false;
    }
}

static uint32_t set_slot(AuAtom a, uint32_t size)
{
    return (uint32_t)(a * 2654435761u) & (size - 1);
}

static void set_insert(HtmlTokenizer *t, AuAtom a)
{
    uint32_t i = set_slot(a, t->attr_set_size);
    while (t->attr_set[i])
        i = (i + 1) & (t->attr_set_size - 1);
    t->attr_set[i] = a;
}

/* Empties the hash set if it holds a tag's names. The set is filled only
 * when a ninth attribute name is looked up, so a tag with eight attributes
 * or fewer never fills it; attr_set_live records whether it was. */
static void set_clear(HtmlTokenizer *t)
{
    if (!t->attr_set_live)
        return;
    memset(t->attr_set, 0, t->attr_set_size * sizeof *t->attr_set);
    t->attr_set_live = false;
}

static bool attr_exists(HtmlTokenizer *t, AuAtom a)
{
    if (t->nattrs < 8) {
        for (uint32_t i = 0; i < t->nattrs; i++)
            if (t->attrs[i].name == a)
                return true;
        return false;
    }
    if (t->nattrs == 8 || t->nattrs * 2 > t->attr_set_size) {
        /* Build or grow the set (the eight names are not in it yet when
         * nattrs is 8; when growing, start over from the attribute list). */
        uint32_t size = t->attr_set_size ? t->attr_set_size : 32;
        while (size < t->nattrs * 4)
            size *= 2;
        if (size != t->attr_set_size) {
            au_free(t->attr_set);
            t->attr_set = au_calloc(size * sizeof *t->attr_set);
            t->attr_set_size = size;
            for (uint32_t i = 0; i < t->nattrs; i++)
                set_insert(t, t->attrs[i].name);
        } else if (!t->attr_set_live) {
            for (uint32_t i = 0; i < t->nattrs; i++)
                set_insert(t, t->attrs[i].name);
        }
        t->attr_set_live = true;
    }
    uint32_t i = set_slot(a, t->attr_set_size);
    while (t->attr_set[i]) {
        if (t->attr_set[i] == a)
            return true;
        i = (i + 1) & (t->attr_set_size - 1);
    }
    return false;
}

static void start_attr(HtmlTokenizer *t)
{
    close_attr(t);
    t->attrname.len = 0;
}

/* Leaving the attribute name state: drop the attribute if its name is
 * already on the tag. */
static void finish_attr_name(HtmlTokenizer *t)
{
    AuAtom a = au_atom(t->attrname.data, t->attrname.len);
    if (attr_exists(t, a)) {
        err(t, "duplicate-attribute");
        t->attr_live = false;
        return;
    }
    if (t->nattrs == t->attr_cap) {
        t->attr_cap = t->attr_cap ? t->attr_cap * 2 : 8;
        t->attrs = au_realloc(t->attrs, t->attr_cap * sizeof *t->attrs);
    }
    HtmlAttr *at = &t->attrs[t->nattrs++];
    at->name = a;
    at->ns = AU_ATTR_NS_NONE;
    at->voff = (uint32_t)t->vals.len;
    at->vlen = 0;
    if (t->nattrs > 8)
        set_insert(t, a);
    t->attr_live = true;
}

static void attr_value_append(HtmlTokenizer *t, const char *s, size_t n)
{
    if (t->attr_live)
        au_buf_append(&t->vals, s, n);
}

static void attr_value_cp(HtmlTokenizer *t, int c)
{
    if (t->attr_live)
        put_cp(&t->vals, c);
}

static void emit_tag(HtmlTokenizer *t)
{
    close_attr(t);
    flush_chars(t);
    HtmlToken tok;
    memset(&tok, 0, sizeof tok);
    tok.name = au_atom(t->name.data, t->name.len);
    tok.self_closing = t->self_closing;
    if (t->end_tag) {
        tok.type = HTML_TOK_END;
        if (t->nattrs)
            err_prev(t, "end-tag-with-attributes");
        if (t->self_closing)
            err_prev(t, "end-tag-with-trailing-solidus");
    } else {
        tok.type = HTML_TOK_START;
        tok.attrs = t->attrs;
        tok.nattrs = t->nattrs;
        tok.vals = t->vals.data;
        t->last_start_tag = tok.name;
    }
    set_clear(t);
    t->emit(t->sink, &tok);
}

static void emit_comment(HtmlTokenizer *t)
{
    flush_chars(t);
    HtmlToken tok;
    memset(&tok, 0, sizeof tok);
    tok.type = HTML_TOK_COMMENT;
    tok.data = t->data.data ? t->data.data : "";
    tok.len = t->data.len;
    t->emit(t->sink, &tok);
}

static void emit_pi(HtmlTokenizer *t)
{
    flush_chars(t);
    HtmlToken tok;
    memset(&tok, 0, sizeof tok);
    tok.type = HTML_TOK_PI;
    tok.name = au_atom(t->tmp.data, t->tmp.len);
    tok.data = t->data.data ? t->data.data : "";
    tok.len = t->data.len;
    t->emit(t->sink, &tok);
}

static void new_doctype(HtmlTokenizer *t)
{
    t->name.len = 0;
    t->pub.len = 0;
    t->sys.len = 0;
    t->doctype_has_name = false;
    t->has_public = t->has_system = false;
    t->force_quirks = false;
}

static void emit_doctype(HtmlTokenizer *t)
{
    flush_chars(t);
    HtmlToken tok;
    memset(&tok, 0, sizeof tok);
    tok.type = HTML_TOK_DOCTYPE;
    tok.has_name = t->doctype_has_name;
    tok.data = t->name.data ? t->name.data : "";
    tok.len = t->name.len;
    tok.has_public = t->has_public;
    tok.pub = t->pub.data ? t->pub.data : "";
    tok.publen = t->pub.len;
    tok.has_system = t->has_system;
    tok.sys = t->sys.data ? t->sys.data : "";
    tok.syslen = t->sys.len;
    tok.force_quirks = t->force_quirks;
    t->emit(t->sink, &tok);
}

static void emit_eof(HtmlTokenizer *t)
{
    flush_chars(t);
    HtmlToken tok;
    memset(&tok, 0, sizeof tok);
    tok.type = HTML_TOK_EOF;
    t->done = true;
    t->emit(t->sink, &tok);
}

/* Converts the temporary buffer to a comment: "?" + buffer. */
static void tmp_to_comment(HtmlTokenizer *t)
{
    t->data.len = 0;
    au_buf_putc(&t->data, '?');
    au_buf_append(&t->data, t->tmp.data, t->tmp.len);
}

static bool in_attr(const HtmlTokenizer *t)
{
    return t->return_state == S_ATTR_VALUE_DQ || t->return_state == S_ATTR_VALUE_SQ ||
           t->return_state == S_ATTR_VALUE_UQ;
}

/* Flushes code points consumed as a character reference. */
static void flush_tmp(HtmlTokenizer *t)
{
    if (in_attr(t))
        attr_value_append(t, t->tmp.data, t->tmp.len);
    else
        au_buf_append(&t->chars, t->tmp.data, t->tmp.len);
}

static bool appropriate_end_tag(const HtmlTokenizer *t)
{
    return t->last_start_tag && au_atom_len(t->last_start_tag) == t->name.len &&
           memcmp(au_atom_str(t->last_start_tag), t->name.data, t->name.len) == 0;
}

/* Emits "</" and the temporary buffer, for an end tag that was not
 * appropriate. */
static void emit_lt_slash_tmp(HtmlTokenizer *t)
{
    au_buf_append(&t->chars, "</", 2);
    au_buf_append(&t->chars, t->tmp.data, t->tmp.len);
}

static bool is_ws(int c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == ' ';
}

static bool is_alnum(int c)
{
    return c >= 0 && c < 0x80 && (au_is_alpha(c) || au_is_digit(c));
}

static bool is_alpha(int c)
{
    return c >= 0 && c < 0x80 && au_is_alpha(c);
}

static bool is_upper(int c)
{
    return c >= 'A' && c <= 'Z';
}

static const uint16_t c1_replacements[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

/* The named character reference state. Returns false to wait for input. */
static bool named_charref(HtmlTokenizer *t)
{
    size_t maxlen = au_html_entity_max_len();
    size_t end = avail_end(t), i = t->pos;
    const char *b = t->buf;
    while (i < end && i - t->pos <= maxlen && is_alnum((unsigned char)b[i]))
        i++;
    size_t run = i - t->pos;
    bool at_end = i == end;
    if (at_end && run <= maxlen && !input_final(t))
        return false;

    int idx = -1;
    size_t k = 0;
    bool semi = false;
    if (run <= maxlen && !at_end && b[i] == ';') {
        idx = au_html_entity_find(b + t->pos, run);
        if (idx >= 0) {
            k = run;
            semi = true;
        }
    }
    if (idx < 0) {
        size_t l = run < 6 ? run : 6;
        for (; l > 0; l--) {
            idx = au_html_entity_find(b + t->pos, l);
            if (idx >= 0 && au_html_entity_legacy(idx)) {
                k = l;
                break;
            }
            idx = -1;
        }
    }
    if (idx < 0) {
        flush_tmp(t);
        t->state = S_AMBIGUOUS_AMP;
        return true;
    }
    if (!semi && in_attr(t)) {
        int next;
        if (t->pos + k < end)
            next = (unsigned char)b[t->pos + k];
        else if (!input_final(t))
            return false;
        else
            next = C_EOF;
        if (next == '=' || is_alnum(next)) {
            attr_value_append(t, "&", 1);
            attr_value_append(t, b + t->pos, k);
            skip_ascii(t, k);
            t->state = t->return_state;
            return true;
        }
    }
    skip_ascii(t, k);
    if (semi)
        skip_ascii(t, 1);
    else
        err(t, "missing-semicolon-after-character-reference");
    uint32_t cp[2];
    int n = au_html_entity_codepoints(idx, cp);
    t->tmp.len = 0;
    for (int j = 0; j < n; j++)
        au_buf_put_utf8(&t->tmp, cp[j]);
    flush_tmp(t);
    t->state = t->return_state;
    return true;
}

static void numeric_charref_end(HtmlTokenizer *t)
{
    uint32_t c = t->charref;
    if (c == 0) {
        err(t, "null-character-reference");
        c = 0xFFFD;
    } else if (c > 0x10FFFF) {
        err(t, "character-reference-outside-unicode-range");
        c = 0xFFFD;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
        err(t, "surrogate-character-reference");
        c = 0xFFFD;
    } else if ((c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE) {
        err(t, "noncharacter-character-reference");
    } else if (c == 0x0D || (c < 0x20 && !is_ws((int)c)) || (c >= 0x7F && c <= 0x9F)) {
        err(t, "control-character-reference");
        if (c >= 0x80 && c <= 0x9F && c1_replacements[c - 0x80])
            c = c1_replacements[c - 0x80];
    }
    t->tmp.len = 0;
    au_buf_put_utf8(&t->tmp, c);
    flush_tmp(t);
    t->state = t->return_state;
}

/* ---- The state machine ---- */

#define GET()                  \
    do {                       \
        c = peek(t, &n);       \
        if (c == C_NEED)       \
            goto suspend;      \
    } while (0)
#define CONSUME() adv(t, c, n)
/* These end the current step; they are blocks rather than do-while
 * statements so that their continue applies to the state loop. */
#define RECONSUME(s)    \
    {                   \
        t->state = (s); \
        continue;       \
    }
#define SWITCH(s)       \
    {                   \
        CONSUME();      \
        t->state = (s); \
        continue;       \
    }

void html_tok_run(HtmlTokenizer *t)
{
    int c;
    size_t n = 1;
    while (!t->pause && !t->done) {
        switch (t->state) {
        case S_DATA:
            scan(t, &t->chars, K_AMP | K_LT);
            GET();
            if (c == '&') {
                t->return_state = S_DATA;
                SWITCH(S_CHARREF);
            } else if (c == '<') {
                SWITCH(S_TAG_OPEN);
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                emit_char(t, 0);
            } else if (c == C_EOF) {
                emit_eof(t);
                continue;
            } else {
                emit_char(t, c);
            }
            CONSUME();
            continue;

        case S_RCDATA:
        case S_RAWTEXT:
        case S_SCRIPT:
        case S_PLAINTEXT: {
            int st = t->state;
            unsigned mask = st == S_RCDATA ? (K_AMP | K_LT) : st == S_PLAINTEXT ? 0 : K_LT;
            scan(t, &t->chars, mask);
            GET();
            if (c == '&' && st == S_RCDATA) {
                t->return_state = S_RCDATA;
                SWITCH(S_CHARREF);
            } else if (c == '<' && st != S_PLAINTEXT) {
                t->text_state = (uint8_t)st;
                SWITCH(st == S_SCRIPT ? S_SCRIPT_LT : S_TEXT_LT);
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                emit_char(t, 0xFFFD);
            } else if (c == C_EOF) {
                emit_eof(t);
                continue;
            } else {
                emit_char(t, c);
            }
            CONSUME();
            continue;
        }

        case S_TAG_OPEN:
            GET();
            if (c == '!') {
                SWITCH(S_MARKUP_DECL);
            } else if (c == '/') {
                SWITCH(S_END_TAG_OPEN);
            } else if (is_alpha(c)) {
                new_tag(t, false);
                RECONSUME(S_TAG_NAME);
            } else if (c == '?') {
                t->tmp.len = 0;
                SWITCH(S_PI_OPEN);
            } else if (c == C_EOF) {
                err(t, "eof-before-tag-name");
                emit_char(t, '<');
                emit_eof(t);
                continue;
            } else {
                err(t, "invalid-first-character-of-tag-name");
                emit_char(t, '<');
                RECONSUME(S_DATA);
            }

        case S_END_TAG_OPEN:
            GET();
            if (is_alpha(c)) {
                new_tag(t, true);
                RECONSUME(S_TAG_NAME);
            } else if (c == '>') {
                err(t, "missing-end-tag-name");
                SWITCH(S_DATA);
            } else if (c == C_EOF) {
                err(t, "eof-before-tag-name");
                au_buf_append(&t->chars, "</", 2);
                emit_eof(t);
                continue;
            } else {
                err(t, "invalid-first-character-of-tag-name");
                t->data.len = 0;
                RECONSUME(S_BOGUS_COMMENT);
            }

        case S_TAG_NAME:
            GET();
            if (is_ws(c)) {
                SWITCH(S_BEFORE_ATTR_NAME);
            } else if (c == '/') {
                SWITCH(S_SELF_CLOSING);
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_tag(t);
                continue;
            } else if (is_upper(c)) {
                au_buf_putc(&t->name, (char)(c + 32));
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                put_cp(&t->name, 0xFFFD);
            } else if (c == C_EOF) {
                err(t, "eof-in-tag");
                emit_eof(t);
                continue;
            } else {
                put_cp(&t->name, c);
            }
            CONSUME();
            continue;

        case S_TEXT_LT:
            GET();
            if (c == '/') {
                t->tmp.len = 0;
                SWITCH(S_TEXT_END_OPEN);
            }
            emit_char(t, '<');
            RECONSUME(t->text_state);

        case S_TEXT_END_OPEN:
            GET();
            if (is_alpha(c)) {
                new_tag(t, true);
                RECONSUME(S_TEXT_END_NAME);
            }
            au_buf_append(&t->chars, "</", 2);
            RECONSUME(t->text_state);

        case S_TEXT_END_NAME:
            GET();
            if (is_ws(c) && appropriate_end_tag(t)) {
                SWITCH(S_BEFORE_ATTR_NAME);
            } else if (c == '/' && appropriate_end_tag(t)) {
                SWITCH(S_SELF_CLOSING);
            } else if (c == '>' && appropriate_end_tag(t)) {
                CONSUME();
                t->state = S_DATA;
                emit_tag(t);
                continue;
            } else if (is_alpha(c)) {
                au_buf_putc(&t->name, (char)au_lower(c));
                au_buf_putc(&t->tmp, (char)c);
                CONSUME();
                continue;
            }
            emit_lt_slash_tmp(t);
            RECONSUME(t->text_state);

        case S_SCRIPT_LT:
            GET();
            if (c == '/') {
                t->tmp.len = 0;
                t->text_state = S_SCRIPT;
                SWITCH(S_TEXT_END_OPEN);
            } else if (c == '!') {
                au_buf_append(&t->chars, "<!", 2);
                SWITCH(S_SCRIPT_ESC_START);
            }
            emit_char(t, '<');
            RECONSUME(S_SCRIPT);

        case S_SCRIPT_ESC_START:
            GET();
            if (c == '-') {
                emit_char(t, '-');
                SWITCH(S_SCRIPT_ESC_START_DASH);
            }
            RECONSUME(S_SCRIPT);

        case S_SCRIPT_ESC_START_DASH:
            GET();
            if (c == '-') {
                emit_char(t, '-');
                SWITCH(S_SCRIPT_ESC_DASH_DASH);
            }
            RECONSUME(S_SCRIPT);

        case S_SCRIPT_ESC:
        case S_SCRIPT_ESC_DASH:
        case S_SCRIPT_ESC_DASH_DASH: {
            int st = t->state;
            if (st == S_SCRIPT_ESC)
                scan(t, &t->chars, K_LT | K_DASH);
            GET();
            if (c == '-') {
                emit_char(t, '-');
                SWITCH(st == S_SCRIPT_ESC ? S_SCRIPT_ESC_DASH : S_SCRIPT_ESC_DASH_DASH);
            } else if (c == '<') {
                SWITCH(S_SCRIPT_ESC_LT);
            } else if (c == '>' && st == S_SCRIPT_ESC_DASH_DASH) {
                emit_char(t, '>');
                SWITCH(S_SCRIPT);
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                emit_char(t, 0xFFFD);
            } else if (c == C_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                emit_eof(t);
                continue;
            } else {
                emit_char(t, c);
            }
            SWITCH(S_SCRIPT_ESC);
        }

        case S_SCRIPT_ESC_LT:
            GET();
            if (c == '/') {
                t->tmp.len = 0;
                t->text_state = S_SCRIPT_ESC;
                SWITCH(S_TEXT_END_OPEN);
            } else if (is_alpha(c)) {
                t->tmp.len = 0;
                emit_char(t, '<');
                RECONSUME(S_SCRIPT_DESC_START);
            }
            emit_char(t, '<');
            RECONSUME(S_SCRIPT_ESC);

        case S_SCRIPT_DESC_START:
        case S_SCRIPT_DESC_END: {
            int st = t->state;
            GET();
            if (is_ws(c) || c == '/' || c == '>') {
                bool script = t->tmp.len == 6 && memcmp(t->tmp.data, "script", 6) == 0;
                emit_char(t, c);
                SWITCH(st == S_SCRIPT_DESC_START ? (script ? S_SCRIPT_DESC : S_SCRIPT_ESC)
                                                 : (script ? S_SCRIPT_ESC : S_SCRIPT_DESC));
            } else if (is_alpha(c)) {
                au_buf_putc(&t->tmp, (char)au_lower(c));
                emit_char(t, c);
                CONSUME();
                continue;
            }
            RECONSUME(st == S_SCRIPT_DESC_START ? S_SCRIPT_ESC : S_SCRIPT_DESC);
        }

        case S_SCRIPT_DESC:
        case S_SCRIPT_DESC_DASH:
        case S_SCRIPT_DESC_DASH_DASH: {
            int st = t->state;
            if (st == S_SCRIPT_DESC)
                scan(t, &t->chars, K_LT | K_DASH);
            GET();
            if (c == '-') {
                emit_char(t, '-');
                SWITCH(st == S_SCRIPT_DESC ? S_SCRIPT_DESC_DASH : S_SCRIPT_DESC_DASH_DASH);
            } else if (c == '<') {
                emit_char(t, '<');
                SWITCH(S_SCRIPT_DESC_LT);
            } else if (c == '>' && st == S_SCRIPT_DESC_DASH_DASH) {
                emit_char(t, '>');
                SWITCH(S_SCRIPT);
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                emit_char(t, 0xFFFD);
            } else if (c == C_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                emit_eof(t);
                continue;
            } else {
                emit_char(t, c);
            }
            SWITCH(S_SCRIPT_DESC);
        }

        case S_SCRIPT_DESC_LT:
            GET();
            if (c == '/') {
                t->tmp.len = 0;
                emit_char(t, '/');
                SWITCH(S_SCRIPT_DESC_END);
            }
            RECONSUME(S_SCRIPT_DESC);

        case S_BEFORE_ATTR_NAME:
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            } else if (c == '/' || c == '>' || c == C_EOF) {
                RECONSUME(S_AFTER_ATTR_NAME);
            } else if (c == '=') {
                err(t, "unexpected-equals-sign-before-attribute-name");
                start_attr(t);
                au_buf_putc(&t->attrname, '=');
                SWITCH(S_ATTR_NAME);
            }
            start_attr(t);
            RECONSUME(S_ATTR_NAME);

        case S_ATTR_NAME:
            GET();
            if (is_ws(c) || c == '/' || c == '>' || c == C_EOF) {
                finish_attr_name(t);
                RECONSUME(S_AFTER_ATTR_NAME);
            } else if (c == '=') {
                finish_attr_name(t);
                SWITCH(S_BEFORE_ATTR_VALUE);
            } else if (is_upper(c)) {
                au_buf_putc(&t->attrname, (char)(c + 32));
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                put_cp(&t->attrname, 0xFFFD);
            } else {
                if (c == '"' || c == '\'' || c == '<')
                    err(t, "unexpected-character-in-attribute-name");
                put_cp(&t->attrname, c);
            }
            CONSUME();
            continue;

        case S_AFTER_ATTR_NAME:
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            } else if (c == '/') {
                SWITCH(S_SELF_CLOSING);
            } else if (c == '=') {
                SWITCH(S_BEFORE_ATTR_VALUE);
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_tag(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-tag");
                emit_eof(t);
                continue;
            }
            start_attr(t);
            RECONSUME(S_ATTR_NAME);

        case S_BEFORE_ATTR_VALUE:
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            } else if (c == '"') {
                SWITCH(S_ATTR_VALUE_DQ);
            } else if (c == '\'') {
                SWITCH(S_ATTR_VALUE_SQ);
            } else if (c == '>') {
                err(t, "missing-attribute-value");
                CONSUME();
                t->state = S_DATA;
                emit_tag(t);
                continue;
            }
            RECONSUME(S_ATTR_VALUE_UQ);

        case S_ATTR_VALUE_DQ:
        case S_ATTR_VALUE_SQ: {
            int st = t->state;
            int q = st == S_ATTR_VALUE_DQ ? '"' : '\'';
            scan(t, t->attr_live ? &t->vals : NULL, K_AMP | (q == '"' ? K_DQ : K_SQ));
            GET();
            if (c == q) {
                SWITCH(S_AFTER_ATTR_VALUE_Q);
            } else if (c == '&') {
                t->return_state = (uint8_t)st;
                SWITCH(S_CHARREF);
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                attr_value_cp(t, 0xFFFD);
            } else if (c == C_EOF) {
                err(t, "eof-in-tag");
                emit_eof(t);
                continue;
            } else {
                attr_value_cp(t, c);
            }
            CONSUME();
            continue;
        }

        case S_ATTR_VALUE_UQ:
            scan(t, t->attr_live ? &t->vals : NULL, K_AMP | K_UQ);
            GET();
            if (is_ws(c)) {
                SWITCH(S_BEFORE_ATTR_NAME);
            } else if (c == '&') {
                t->return_state = S_ATTR_VALUE_UQ;
                SWITCH(S_CHARREF);
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_tag(t);
                continue;
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                attr_value_cp(t, 0xFFFD);
            } else if (c == C_EOF) {
                err(t, "eof-in-tag");
                emit_eof(t);
                continue;
            } else {
                if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`')
                    err(t, "unexpected-character-in-unquoted-attribute-value");
                attr_value_cp(t, c);
            }
            CONSUME();
            continue;

        case S_AFTER_ATTR_VALUE_Q:
            GET();
            if (is_ws(c)) {
                SWITCH(S_BEFORE_ATTR_NAME);
            } else if (c == '/') {
                SWITCH(S_SELF_CLOSING);
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_tag(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-tag");
                emit_eof(t);
                continue;
            }
            err(t, "missing-whitespace-between-attributes");
            RECONSUME(S_BEFORE_ATTR_NAME);

        case S_SELF_CLOSING:
            GET();
            if (c == '>') {
                t->self_closing = true;
                CONSUME();
                t->state = S_DATA;
                emit_tag(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-tag");
                emit_eof(t);
                continue;
            }
            err(t, "unexpected-solidus-in-tag");
            RECONSUME(S_BEFORE_ATTR_NAME);

        case S_BOGUS_COMMENT:
            scan(t, &t->data, K_GT);
            GET();
            if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_comment(t);
                continue;
            } else if (c == C_EOF) {
                emit_comment(t);
                emit_eof(t);
                continue;
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                put_cp(&t->data, 0xFFFD);
            } else {
                put_cp(&t->data, c);
            }
            CONSUME();
            continue;

        case S_MARKUP_DECL: {
            peek(t, &n); /* reports input-stream errors in order */
            int r = lookahead(t, "--", false);
            if (r == C_NEED)
                goto suspend;
            if (r) {
                skip_ascii(t, 2);
                t->data.len = 0;
                t->state = S_COMMENT_START;
                continue;
            }
            r = lookahead(t, "doctype", true);
            if (r == C_NEED)
                goto suspend;
            if (r) {
                skip_ascii(t, 7);
                t->state = S_DOCTYPE;
                continue;
            }
            r = lookahead(t, "[CDATA[", false);
            if (r == C_NEED)
                goto suspend;
            if (r) {
                skip_ascii(t, 6);
                bool ok = t->cdata_ok && t->cdata_ok(t->sink);
                if (!ok)
                    err(t, "cdata-in-html-content");
                skip_ascii(t, 1);
                if (ok) {
                    t->state = S_CDATA;
                } else {
                    t->data.len = 0;
                    au_buf_puts(&t->data, "[CDATA[");
                    t->state = S_BOGUS_COMMENT;
                }
                continue;
            }
            err(t, "incorrectly-opened-comment");
            t->data.len = 0;
            t->state = S_BOGUS_COMMENT;
            continue;
        }

        case S_COMMENT_START:
            GET();
            if (c == '-') {
                SWITCH(S_COMMENT_START_DASH);
            } else if (c == '>') {
                err(t, "abrupt-closing-of-empty-comment");
                CONSUME();
                t->state = S_DATA;
                emit_comment(t);
                continue;
            }
            RECONSUME(S_COMMENT);

        case S_COMMENT_START_DASH:
            GET();
            if (c == '-') {
                SWITCH(S_COMMENT_END);
            } else if (c == '>') {
                err(t, "abrupt-closing-of-empty-comment");
                CONSUME();
                t->state = S_DATA;
                emit_comment(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-comment");
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            au_buf_putc(&t->data, '-');
            RECONSUME(S_COMMENT);

        case S_COMMENT:
            scan(t, &t->data, K_LT | K_DASH);
            GET();
            if (c == '<') {
                au_buf_putc(&t->data, '<');
                SWITCH(S_COMMENT_LT);
            } else if (c == '-') {
                SWITCH(S_COMMENT_END_DASH);
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                put_cp(&t->data, 0xFFFD);
            } else if (c == C_EOF) {
                err(t, "eof-in-comment");
                emit_comment(t);
                emit_eof(t);
                continue;
            } else {
                put_cp(&t->data, c);
            }
            CONSUME();
            continue;

        case S_COMMENT_LT:
            GET();
            if (c == '!') {
                au_buf_putc(&t->data, '!');
                SWITCH(S_COMMENT_LT_BANG);
            } else if (c == '<') {
                au_buf_putc(&t->data, '<');
                CONSUME();
                continue;
            }
            RECONSUME(S_COMMENT);

        case S_COMMENT_LT_BANG:
            GET();
            if (c == '-')
                SWITCH(S_COMMENT_LT_BANG_DASH);
            RECONSUME(S_COMMENT);

        case S_COMMENT_LT_BANG_DASH:
            GET();
            if (c == '-')
                SWITCH(S_COMMENT_LT_BANG_DASH_DASH);
            RECONSUME(S_COMMENT_END_DASH);

        case S_COMMENT_LT_BANG_DASH_DASH:
            GET();
            if (c != '>' && c != C_EOF)
                err(t, "nested-comment");
            RECONSUME(S_COMMENT_END);

        case S_COMMENT_END_DASH:
            GET();
            if (c == '-') {
                SWITCH(S_COMMENT_END);
            } else if (c == C_EOF) {
                err(t, "eof-in-comment");
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            au_buf_putc(&t->data, '-');
            RECONSUME(S_COMMENT);

        case S_COMMENT_END:
            GET();
            if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_comment(t);
                continue;
            } else if (c == '!') {
                SWITCH(S_COMMENT_END_BANG);
            } else if (c == '-') {
                au_buf_putc(&t->data, '-');
                CONSUME();
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-comment");
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            au_buf_append(&t->data, "--", 2);
            RECONSUME(S_COMMENT);

        case S_COMMENT_END_BANG:
            GET();
            if (c == '-') {
                au_buf_append(&t->data, "--!", 3);
                SWITCH(S_COMMENT_END_DASH);
            } else if (c == '>') {
                err(t, "incorrectly-closed-comment");
                CONSUME();
                t->state = S_DATA;
                emit_comment(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-comment");
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            au_buf_append(&t->data, "--!", 3);
            RECONSUME(S_COMMENT);

        case S_DOCTYPE:
            GET();
            if (is_ws(c)) {
                SWITCH(S_BEFORE_DOCTYPE_NAME);
            } else if (c == '>') {
                RECONSUME(S_BEFORE_DOCTYPE_NAME);
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                new_doctype(t);
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            err(t, "missing-whitespace-before-doctype-name");
            RECONSUME(S_BEFORE_DOCTYPE_NAME);

        case S_BEFORE_DOCTYPE_NAME:
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            } else if (c == '>') {
                err(t, "missing-doctype-name");
                new_doctype(t);
                t->force_quirks = true;
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                new_doctype(t);
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            new_doctype(t);
            t->doctype_has_name = true;
            if (is_upper(c)) {
                au_buf_putc(&t->name, (char)(c + 32));
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                put_cp(&t->name, 0xFFFD);
            } else {
                put_cp(&t->name, c);
            }
            SWITCH(S_DOCTYPE_NAME);

        case S_DOCTYPE_NAME:
            GET();
            if (is_ws(c)) {
                SWITCH(S_AFTER_DOCTYPE_NAME);
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (is_upper(c)) {
                au_buf_putc(&t->name, (char)(c + 32));
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                put_cp(&t->name, 0xFFFD);
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            } else {
                put_cp(&t->name, c);
            }
            CONSUME();
            continue;

        case S_AFTER_DOCTYPE_NAME: {
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            int r = lookahead(t, "public", true);
            if (r == C_NEED)
                goto suspend;
            if (r) {
                skip_ascii(t, 6);
                t->state = S_AFTER_DOCTYPE_PUBLIC_KW;
                continue;
            }
            r = lookahead(t, "system", true);
            if (r == C_NEED)
                goto suspend;
            if (r) {
                skip_ascii(t, 6);
                t->state = S_AFTER_DOCTYPE_SYSTEM_KW;
                continue;
            }
            err(t, "invalid-character-sequence-after-doctype-name");
            t->force_quirks = true;
            RECONSUME(S_BOGUS_DOCTYPE);
        }

        case S_AFTER_DOCTYPE_PUBLIC_KW:
        case S_AFTER_DOCTYPE_SYSTEM_KW: {
            bool pub = t->state == S_AFTER_DOCTYPE_PUBLIC_KW;
            GET();
            if (is_ws(c)) {
                SWITCH(pub ? S_BEFORE_DOCTYPE_PUBLIC_ID : S_BEFORE_DOCTYPE_SYSTEM_ID);
            } else if (c == '"' || c == '\'') {
                err(t, pub ? "missing-whitespace-after-doctype-public-keyword"
                           : "missing-whitespace-after-doctype-system-keyword");
                if (pub) {
                    t->has_public = true;
                    t->pub.len = 0;
                    SWITCH(c == '"' ? S_DOCTYPE_PUBLIC_ID_DQ : S_DOCTYPE_PUBLIC_ID_SQ);
                }
                t->has_system = true;
                t->sys.len = 0;
                SWITCH(c == '"' ? S_DOCTYPE_SYSTEM_ID_DQ : S_DOCTYPE_SYSTEM_ID_SQ);
            } else if (c == '>') {
                err(t, pub ? "missing-doctype-public-identifier" : "missing-doctype-system-identifier");
                t->force_quirks = true;
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            err(t, pub ? "missing-quote-before-doctype-public-identifier"
                       : "missing-quote-before-doctype-system-identifier");
            t->force_quirks = true;
            RECONSUME(S_BOGUS_DOCTYPE);
        }

        case S_BEFORE_DOCTYPE_PUBLIC_ID:
        case S_BEFORE_DOCTYPE_SYSTEM_ID: {
            bool pub = t->state == S_BEFORE_DOCTYPE_PUBLIC_ID;
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            } else if (c == '"' || c == '\'') {
                if (pub) {
                    t->has_public = true;
                    t->pub.len = 0;
                    SWITCH(c == '"' ? S_DOCTYPE_PUBLIC_ID_DQ : S_DOCTYPE_PUBLIC_ID_SQ);
                }
                t->has_system = true;
                t->sys.len = 0;
                SWITCH(c == '"' ? S_DOCTYPE_SYSTEM_ID_DQ : S_DOCTYPE_SYSTEM_ID_SQ);
            } else if (c == '>') {
                err(t, pub ? "missing-doctype-public-identifier" : "missing-doctype-system-identifier");
                t->force_quirks = true;
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            err(t, pub ? "missing-quote-before-doctype-public-identifier"
                       : "missing-quote-before-doctype-system-identifier");
            t->force_quirks = true;
            RECONSUME(S_BOGUS_DOCTYPE);
        }

        case S_DOCTYPE_PUBLIC_ID_DQ:
        case S_DOCTYPE_PUBLIC_ID_SQ:
        case S_DOCTYPE_SYSTEM_ID_DQ:
        case S_DOCTYPE_SYSTEM_ID_SQ: {
            int st = t->state;
            bool pub = st == S_DOCTYPE_PUBLIC_ID_DQ || st == S_DOCTYPE_PUBLIC_ID_SQ;
            int q = (st == S_DOCTYPE_PUBLIC_ID_DQ || st == S_DOCTYPE_SYSTEM_ID_DQ) ? '"' : '\'';
            AuBuf *b = pub ? &t->pub : &t->sys;
            GET();
            if (c == q) {
                SWITCH(pub ? S_AFTER_DOCTYPE_PUBLIC_ID : S_AFTER_DOCTYPE_SYSTEM_ID);
            } else if (c == 0) {
                err(t, "unexpected-null-character");
                put_cp(b, 0xFFFD);
            } else if (c == '>') {
                err(t, pub ? "abrupt-doctype-public-identifier" : "abrupt-doctype-system-identifier");
                t->force_quirks = true;
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            } else {
                put_cp(b, c);
            }
            CONSUME();
            continue;
        }

        case S_AFTER_DOCTYPE_PUBLIC_ID:
        case S_BETWEEN_DOCTYPE_IDS: {
            bool after = t->state == S_AFTER_DOCTYPE_PUBLIC_ID;
            GET();
            if (is_ws(c)) {
                if (after)
                    SWITCH(S_BETWEEN_DOCTYPE_IDS);
                CONSUME();
                continue;
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == '"' || c == '\'') {
                if (after)
                    err(t, "missing-whitespace-between-doctype-public-and-system-identifiers");
                t->has_system = true;
                t->sys.len = 0;
                SWITCH(c == '"' ? S_DOCTYPE_SYSTEM_ID_DQ : S_DOCTYPE_SYSTEM_ID_SQ);
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            err(t, "missing-quote-before-doctype-system-identifier");
            t->force_quirks = true;
            RECONSUME(S_BOGUS_DOCTYPE);
        }

        case S_AFTER_DOCTYPE_SYSTEM_ID:
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-doctype");
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            err(t, "unexpected-character-after-doctype-system-identifier");
            RECONSUME(S_BOGUS_DOCTYPE);

        case S_BOGUS_DOCTYPE:
            GET();
            if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_doctype(t);
                continue;
            } else if (c == 0) {
                err(t, "unexpected-null-character");
            } else if (c == C_EOF) {
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            CONSUME();
            continue;

        case S_CDATA:
            scan(t, &t->chars, K_RB);
            GET();
            if (c == ']') {
                SWITCH(S_CDATA_BRACKET);
            } else if (c == C_EOF) {
                err(t, "eof-in-cdata");
                emit_eof(t);
                continue;
            }
            emit_char(t, c);
            CONSUME();
            continue;

        case S_CDATA_BRACKET:
            GET();
            if (c == ']')
                SWITCH(S_CDATA_END);
            emit_char(t, ']');
            RECONSUME(S_CDATA);

        case S_CDATA_END:
            GET();
            if (c == ']') {
                emit_char(t, ']');
                CONSUME();
                continue;
            } else if (c == '>') {
                SWITCH(S_DATA);
            }
            au_buf_append(&t->chars, "]]", 2);
            RECONSUME(S_CDATA);

        case S_PI_OPEN:
            GET();
            if (is_alpha(c) || c == '_') {
                RECONSUME(S_PI_TARGET);
            } else if (c == C_EOF) {
                err(t, "eof-in-processing-instruction");
                emit_eof(t);
                continue;
            }
            err(t, "invalid-first-character-of-processing-instruction-target");
            tmp_to_comment(t);
            RECONSUME(S_BOGUS_COMMENT);

        case S_PI_TARGET:
            GET();
            if (is_ws(c) || c == '?' || c == '>') {
                if (au_ieq(t->tmp.data, t->tmp.len, "xml", 3) ||
                    au_ieq(t->tmp.data, t->tmp.len, "xml-stylesheet", 14)) {
                    err(t, "disallowed-processing-instruction-target");
                    tmp_to_comment(t);
                    RECONSUME(S_BOGUS_COMMENT);
                }
                t->data.len = 0;
                t->pi_line = t->line;
                t->pi_col = t->col;
                RECONSUME(S_AFTER_PI_TARGET);
            } else if (is_alnum(c) || c == '-' || c == '_') {
                au_buf_putc(&t->tmp, (char)c);
                CONSUME();
                continue;
            } else if (c == C_EOF) {
                err(t, "eof-in-processing-instruction");
                emit_eof(t);
                continue;
            }
            err(t, "invalid-processing-instruction-target");
            tmp_to_comment(t);
            RECONSUME(S_BOGUS_COMMENT);

        case S_AFTER_PI_TARGET:
            GET();
            if (is_ws(c)) {
                CONSUME();
                continue;
            }
            RECONSUME(S_PI_DATA);

        case S_PI_DATA:
            GET();
            if (c == '?') {
                SWITCH(S_PI_QUESTION);
            } else if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_pi(t);
                continue;
            } else if (c == C_EOF) {
                err_pi_eof(t);
                emit_eof(t);
                continue;
            }
            put_cp(&t->data, c);
            CONSUME();
            continue;

        case S_PI_QUESTION:
            GET();
            if (c == '>') {
                CONSUME();
                t->state = S_DATA;
                emit_pi(t);
                continue;
            } else if (c == C_EOF) {
                err_pi_eof(t);
                emit_eof(t);
                continue;
            }
            au_buf_putc(&t->data, '?');
            RECONSUME(S_PI_DATA);

        case S_CHARREF:
            t->tmp.len = 0;
            au_buf_putc(&t->tmp, '&');
            GET();
            if (is_alnum(c)) {
                RECONSUME(S_NAMED_CHARREF);
            } else if (c == '#') {
                au_buf_putc(&t->tmp, '#');
                SWITCH(S_NUMERIC_CHARREF);
            }
            flush_tmp(t);
            RECONSUME(t->return_state);

        case S_NAMED_CHARREF:
            if (!named_charref(t))
                goto suspend;
            continue;

        case S_AMBIGUOUS_AMP:
            GET();
            if (is_alnum(c)) {
                if (in_attr(t))
                    attr_value_cp(t, c);
                else
                    emit_char(t, c);
                CONSUME();
                continue;
            } else if (c == ';') {
                err(t, "unknown-named-character-reference");
            }
            RECONSUME(t->return_state);

        case S_NUMERIC_CHARREF:
            t->charref = 0;
            GET();
            if (c == 'x' || c == 'X') {
                au_buf_putc(&t->tmp, (char)c);
                SWITCH(S_HEX_CHARREF_START);
            } else if (c >= '0' && c <= '9') {
                RECONSUME(S_DEC_CHARREF);
            }
            err(t, "absence-of-digits-in-numeric-character-reference");
            flush_tmp(t);
            RECONSUME(t->return_state);

        case S_HEX_CHARREF_START:
            GET();
            if (c >= 0 && c < 0x80 && au_is_hex(c))
                RECONSUME(S_HEX_CHARREF);
            err(t, "absence-of-digits-in-numeric-character-reference");
            flush_tmp(t);
            RECONSUME(t->return_state);

        case S_HEX_CHARREF:
        case S_DEC_CHARREF: {
            bool hex = t->state == S_HEX_CHARREF;
            GET();
            int v = -1;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (hex && c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            if (v >= 0) {
                /* Saturate: anything past 0x10FFFF is out of range anyway. */
                if (t->charref <= 0x10FFFF)
                    t->charref = t->charref * (hex ? 16 : 10) + (uint32_t)v;
                CONSUME();
                continue;
            } else if (c == ';') {
                SWITCH(S_NUMERIC_CHARREF_END);
            }
            err(t, "missing-semicolon-after-character-reference");
            RECONSUME(S_NUMERIC_CHARREF_END);
        }

        case S_NUMERIC_CHARREF_END:
            numeric_charref_end(t);
            continue;

        default:
            t->state = S_DATA;
            continue;
        }
    }
    flush_chars(t);
    return;
suspend:
    flush_chars(t);
}
