/* The Page Source window's model (see sourceview.h). */
#include "sourceview.h"

#include <string.h>

/* ---- The colouring tokenizer ----
 * One byte of state: the low 4 bits say where the scan is, the next two
 * which raw-text element the current tag opens (so that after <script ...>
 * the text up to </script is not coloured), and one bit that the tag is
 * an end tag. */
enum {
    S_TEXT,     /* between tags */
    S_TAG_OPEN, /* after '<': a name, '/', '!' or text follows */
    S_TAG_NAME, /* in a tag's name */
    S_TAG,      /* in a tag, between attributes */
    S_ATTR,     /* in an attribute name */
    S_AFTER_EQ, /* after '=': a value follows */
    S_VALUE_DQ, /* in a "quoted" value */
    S_VALUE_SQ, /* in a 'quoted' value */
    S_VALUE,    /* in an unquoted value */
    S_BANG,     /* in <!DOCTYPE ...> or another <! declaration */
    S_COMMENT,  /* in <!-- ... --> */
    S_RAW       /* in the text of <script> or <style> */
};
enum { RAW_NONE = 0, RAW_SCRIPT = 1, RAW_STYLE = 2 };
#define ST(s) ((s)&15)
#define RAW(s) (((s) >> 4) & 3)
#define MAKE(st, raw) ((uint8_t)((st) | ((raw) << 4)))

static bool name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == ':' ||
           c == '_' || c >= 0x80;
}

static bool space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool starts_ci(const char *p, const char *end, const char *word)
{
    for (; *word; word++, p++) {
        if (p >= end)
            return false;
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != *word)
            return false;
    }
    return true;
}

/* Consumes the byte at p (end is the end of the document) in state and
 * returns the state after it; *style is the byte's own style. */
static uint8_t advance(uint8_t state, const char *p, const char *end, int *style)
{
    unsigned char c = (unsigned char)*p;
    int raw = RAW(state);
    switch (ST(state)) {
    case S_TEXT:
        if (c == '<' && p + 1 < end) {
            unsigned char n = (unsigned char)p[1];
            if (n == '!') {
                bool comment = p + 3 < end && p[2] == '-' && p[3] == '-';
                *style = comment ? AU_TS_COMMENT : AU_TS_DOCTYPE;
                return MAKE(comment ? S_COMMENT : S_BANG, 0);
            }
            if (n == '/' || n == '?' || (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z')) {
                *style = AU_TS_TAG;
                return MAKE(S_TAG_OPEN, 0);
            }
        }
        *style = AU_TS_PLAIN;
        return state;
    case S_TAG_OPEN:
        *style = AU_TS_TAG;
        if (c == '/' || c == '?')
            return state;
        if (c == '>')
            return MAKE(S_TEXT, 0);
        /* The name starts here: remember a raw-text element. (A '<' or
         * '/' always precedes this byte.) */
        raw = starts_ci(p, end, "script") && (p + 6 >= end || !name_char((unsigned char)p[6]))  ? RAW_SCRIPT
              : starts_ci(p, end, "style") && (p + 5 >= end || !name_char((unsigned char)p[5])) ? RAW_STYLE
                                                                                                 : RAW_NONE;
        if (p[-1] == '/')
            raw = RAW_NONE; /* an end tag opens nothing */
        return MAKE(S_TAG_NAME, raw);
    case S_TAG_NAME:
        *style = AU_TS_TAG;
        if (c == '>')
            return MAKE(raw ? S_RAW : S_TEXT, raw);
        if (space(c) || c == '/')
            return MAKE(S_TAG, raw);
        return state;
    case S_TAG:
        if (c == '>') {
            *style = AU_TS_TAG;
            return MAKE(raw ? S_RAW : S_TEXT, raw);
        }
        if (space(c) || c == '/') {
            *style = AU_TS_TAG;
            return state;
        }
        if (c == '=') {
            *style = AU_TS_TAG;
            return MAKE(S_AFTER_EQ, raw);
        }
        *style = AU_TS_ATTR;
        return MAKE(S_ATTR, raw);
    case S_ATTR:
        if (c == '=') {
            *style = AU_TS_TAG;
            return MAKE(S_AFTER_EQ, raw);
        }
        if (c == '>') {
            *style = AU_TS_TAG;
            return MAKE(raw ? S_RAW : S_TEXT, raw);
        }
        if (space(c) || c == '/') {
            *style = AU_TS_TAG;
            return MAKE(S_TAG, raw);
        }
        *style = AU_TS_ATTR;
        return state;
    case S_AFTER_EQ:
        if (space(c)) {
            *style = AU_TS_TAG;
            return state;
        }
        if (c == '>') {
            *style = AU_TS_TAG;
            return MAKE(raw ? S_RAW : S_TEXT, raw);
        }
        *style = AU_TS_STRING;
        return MAKE(c == '"' ? S_VALUE_DQ : c == '\'' ? S_VALUE_SQ : S_VALUE, raw);
    case S_VALUE_DQ:
    case S_VALUE_SQ:
        *style = AU_TS_STRING;
        if (c == (ST(state) == S_VALUE_DQ ? '"' : '\''))
            return MAKE(S_TAG, raw);
        return state;
    case S_VALUE:
        if (c == '>') {
            *style = AU_TS_TAG;
            return MAKE(raw ? S_RAW : S_TEXT, raw);
        }
        if (space(c)) {
            *style = AU_TS_TAG;
            return MAKE(S_TAG, raw);
        }
        *style = AU_TS_STRING;
        return state;
    case S_BANG:
        *style = AU_TS_DOCTYPE;
        return c == '>' ? MAKE(S_TEXT, 0) : state;
    case S_COMMENT:
        *style = AU_TS_COMMENT;
        /* "-->" ends it: this byte is the '>' after two dashes. */
        if (c == '>' && p[-1] == '-' && p[-2] == '-')
            return MAKE(S_TEXT, 0);
        return state;
    case S_RAW:
        if (c == '<' && p + 1 < end && p[1] == '/' && starts_ci(p + 2, end, raw == RAW_SCRIPT ? "script" : "style")) {
            *style = AU_TS_TAG;
            return MAKE(S_TAG_OPEN, 0);
        }
        *style = AU_TS_PLAIN;
        return state;
    default:
        *style = AU_TS_PLAIN;
        return MAKE(S_TEXT, 0);
    }
}

/* ---- The index ---- */

void au_sourceview_init(AuSourceView *sv, const char *data, size_t len)
{
    memset(sv, 0, sizeof *sv);
    /* Offsets are 32 bits; a document is far smaller. */
    if (len > 0x7FFFFFFFu)
        len = 0x7FFFFFFFu;
    sv->data = au_alloc(len + 1);
    memcpy(sv->data, data, len);
    sv->data[len] = 0;
    sv->len = len;
    size_t cap = len / 40 + 16;
    sv->segs = au_alloc(cap * sizeof *sv->segs);
    const char *p = sv->data, *end = sv->data + len;
    uint8_t state = MAKE(S_TEXT, 0);
    uint32_t number = 1;
    bool first = true; /* the next segment starts a line */
    while (p < end || first) {
        if (sv->nlines + 2 > cap) {
            cap *= 2;
            sv->segs = au_realloc(sv->segs, cap * sizeof *sv->segs);
        }
        AuSourceSeg *g = &sv->segs[sv->nlines++];
        g->start = (uint32_t)(p - sv->data);
        g->number = first ? number : 0;
        sv->nnumbers += first;
        g->state = state;
        /* The segment runs to the line break or the size limit (at a
         * character boundary). */
        const char *q = p;
        uint32_t cell = 0;
        bool broke = false;
        while (q < end) {
            unsigned char c = (unsigned char)*q;
            if (c == '\n' || c == '\r')
                break;
            if (q - p >= AU_SOURCE_SEG_MAX && (c & 0xC0) != 0x80) {
                broke = true;
                break;
            }
            int style;
            state = advance(state, q, end, &style);
            if (c == '\t')
                cell += 4 - cell % 4;
            else if ((c & 0xC0) != 0x80)
                cell++;
            q++;
        }
        g->cells = cell > 0xFFFF ? 0xFFFF : (uint16_t)cell;
        first = false;
        if (!broke && q < end) {
            /* The line break: CR LF, CR or LF. The tokenizer sees it too
             * (it ends an unquoted value, and may sit in a comment). */
            int style;
            state = advance(state, q, end, &style);
            if (*q == '\r' && q + 1 < end && q[1] == '\n') {
                q++;
                state = advance(state, q, end, &style);
            }
            q++;
            number++;
            first = true;
            if (q >= end) {
                /* A final line break starts no further line. */
                first = false;
            }
        }
        p = q;
    }
    sv->segs[sv->nlines].start = (uint32_t)len;
    sv->segs[sv->nlines].number = 0;
    sv->segs[sv->nlines].state = state;
    sv->segs[sv->nlines].cells = 0;
}

void au_sourceview_free(AuSourceView *sv)
{
    au_free(sv->data);
    au_free(sv->segs);
    memset(sv, 0, sizeof *sv);
}

const char *au_sourceview_line(const AuSourceView *sv, uint32_t i, size_t *len)
{
    if (i >= sv->nlines) {
        *len = 0;
        return "";
    }
    size_t s = sv->segs[i].start, e = sv->segs[i + 1].start;
    /* Without the line break. */
    while (e > s && (sv->data[e - 1] == '\n' || sv->data[e - 1] == '\r'))
        e--;
    *len = e - s;
    return sv->data + s;
}

int au_sourceview_runs(const AuSourceView *sv, uint32_t i, size_t from, size_t to, AuSourceRun *out, int max)
{
    if (i >= sv->nlines || max < 1)
        return 0;
    size_t len;
    const char *line = au_sourceview_line(sv, i, &len);
    if (to > len)
        to = len;
    const char *end = sv->data + sv->len;
    uint8_t state = sv->segs[i].state;
    int n = 0, last = -1;
    for (size_t k = 0; k < to; k++) {
        int style;
        state = advance(state, line + k, end, &style);
        if (k < from)
            continue;
        if (style != last) {
            if (n == max)
                break;
            out[n].start = (uint32_t)k;
            out[n].style = (uint8_t)style;
            n++;
            last = style;
        }
    }
    return n;
}

const char *au_source_style_name(int style)
{
    static const char *const names[] = {"text", "tag", "attr", "string", "comment", "doctype"};
    return style >= 0 && style < 6 ? names[style] : "text";
}

/* ---- As a text view's source ---- */

static uint32_t sv_count(void *ctx)
{
    return ((AuSourceView *)ctx)->nlines;
}

static void sv_line(void *ctx, uint32_t i, AuTextLine *out)
{
    AuSourceView *sv = ctx;
    out->text = au_sourceview_line(sv, i, &out->len);
    out->cells = sv->segs[i].cells;
    out->known_cells = sv->segs[i].cells != 0xFFFF;
    out->number = sv->segs[i].number;
    out->joined = i > 0 && sv->segs[i].number == 0;
    out->style = AU_TS_PLAIN;
}

static int sv_runs(void *ctx, uint32_t i, size_t from, size_t to, AuTextRun *out, int max)
{
    return au_sourceview_runs(ctx, i, from, to, out, max);
}

void au_sourceview_source(AuSourceView *sv, AuTextSource *out)
{
    memset(out, 0, sizeof *out);
    out->ctx = sv;
    out->count = sv_count;
    out->line = sv_line;
    out->runs = sv_runs;
}
