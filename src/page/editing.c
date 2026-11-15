/* The text editing model of form fields (see editing.h). */
#include "editing.h"

#include <string.h>

void au_edit_range(const AuControl *c, uint32_t *start, uint32_t *end)
{
    uint32_t a = c->anchor, b = c->caret;
    if (a > c->value.len)
        a = (uint32_t)c->value.len;
    if (b > c->value.len)
        b = (uint32_t)c->value.len;
    *start = a < b ? a : b;
    *end = a < b ? b : a;
}

bool au_edit_has_selection(const AuControl *c)
{
    return c->anchor != c->caret;
}

static bool is_cont(unsigned char ch)
{
    return (ch & 0xC0) == 0x80;
}

static uint32_t cp_at(const char *s, size_t len, uint32_t off)
{
    size_t adv;
    return off < len ? au_utf8_decode(s + off, len - off, &adv) : 0;
}

static bool is_mark(uint32_t cp)
{
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
           cp == 0x200D;
}

static uint32_t prev_cp(const char *s, uint32_t off)
{
    if (off == 0)
        return 0;
    off--;
    while (off > 0 && is_cont((unsigned char)s[off]))
        off--;
    return off;
}

static uint32_t next_cp(const char *s, size_t len, uint32_t off)
{
    if (off >= len)
        return (uint32_t)len;
    off++;
    while (off < len && is_cont((unsigned char)s[off]))
        off++;
    return off;
}

uint32_t au_edit_prev_char(const char *s, size_t len, uint32_t off)
{
    if (off > len)
        off = (uint32_t)len;
    while (off > 0) {
        off = prev_cp(s, off);
        if (!is_mark(cp_at(s, len, off)))
            break;
    }
    return off;
}

uint32_t au_edit_next_char(const char *s, size_t len, uint32_t off)
{
    off = next_cp(s, len, off);
    while (off < len && is_mark(cp_at(s, len, off)))
        off = next_cp(s, len, off);
    return off;
}

/* Letters, digits, and everything outside ASCII except general
 * punctuation and spaces, plus apostrophes inside words are not handled:
 * a word stops at them. */
static bool is_word(uint32_t cp)
{
    if (cp < 0x80)
        return au_is_alpha((int)cp) || au_is_digit((int)cp) || cp == '_';
    if (cp == 0xA0 || (cp >= 0x2000 && cp <= 0x206F) || cp == 0x3000 || (cp >= 0x3001 && cp <= 0x3003) ||
        cp == 0xAB || cp == 0xBB || cp == 0xBF || cp == 0xA1)
        return false;
    return true;
}

uint32_t au_edit_prev_word(const char *s, size_t len, uint32_t off)
{
    if (off > len)
        off = (uint32_t)len;
    while (off > 0) {
        uint32_t p = prev_cp(s, off);
        if (is_word(cp_at(s, len, p)))
            break;
        off = p;
    }
    while (off > 0) {
        uint32_t p = prev_cp(s, off);
        if (!is_word(cp_at(s, len, p)) && !is_mark(cp_at(s, len, p)))
            break;
        off = p;
    }
    return off;
}

uint32_t au_edit_next_word(const char *s, size_t len, uint32_t off)
{
    while (off < len && !is_word(cp_at(s, len, off)))
        off = next_cp(s, len, off);
    while (off < len && (is_word(cp_at(s, len, off)) || is_mark(cp_at(s, len, off))))
        off = next_cp(s, len, off);
    return off;
}

void au_edit_word_at(const char *s, size_t len, uint32_t off, uint32_t *start, uint32_t *end)
{
    if (off > len)
        off = (uint32_t)len;
    if (len == 0) {
        *start = *end = 0;
        return;
    }
    /* At the very end, take the character before. */
    uint32_t probe = off == len ? prev_cp(s, off) : off;
    bool w = is_word(cp_at(s, len, probe));
    uint32_t a = probe, b = probe;
    while (a > 0) {
        uint32_t p = prev_cp(s, a);
        uint32_t cp = cp_at(s, len, p);
        if (cp == '\n' || is_word(cp) != w)
            break;
        a = p;
    }
    while (b < len) {
        uint32_t cp = cp_at(s, len, b);
        if ((cp == '\n' && b != probe) || is_word(cp) != w)
            break;
        b = next_cp(s, len, b);
        if (cp == '\n')
            break;
    }
    *start = a;
    *end = b;
}

void au_edit_line_at(const char *s, size_t len, uint32_t off, uint32_t *start, uint32_t *end)
{
    if (off > len)
        off = (uint32_t)len;
    uint32_t a = off, b = off;
    while (a > 0 && s[a - 1] != '\n')
        a--;
    while (b < len && s[b] != '\n')
        b++;
    if (b < len)
        b++; /* the line break with it */
    *start = a;
    *end = b;
}

/* Saves the state before an edit of kind, unless it continues a run. */
static void snapshot(AuControl *c, int kind)
{
    if (kind != AU_EDIT_OTHER && c->has_undo && c->undo_kind == kind && !au_edit_has_selection(c))
        return;
    au_buf_clear(&c->undo);
    au_buf_append(&c->undo, c->value.data ? c->value.data : "", c->value.len);
    c->undo_anchor = c->anchor;
    c->undo_caret = c->caret;
    c->has_undo = true;
    c->undo_kind = (uint8_t)kind;
}

static void replace(AuControl *c, uint32_t a, uint32_t b, const char *s, size_t n)
{
    size_t tail = c->value.len - b;
    au_buf_reserve(&c->value, n + 1);
    char *d = c->value.data;
    memmove(d + a + n, d + b, tail);
    if (n)
        memcpy(d + a, s, n);
    c->value.len = a + n + tail;
    au_buf_cstr(&c->value);
    c->anchor = c->caret = (uint32_t)(a + n);
    c->dirty = true;
}

bool au_edit_insert(AuControl *c, const char *s, size_t n, long max_chars, bool single_line, int kind)
{
    uint32_t a, b;
    au_edit_range(c, &a, &b);
    /* Clean the text: no NULs; line breaks as LF (spaces on one line). */
    AuBuf t;
    au_buf_init(&t);
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == 0)
            continue;
        if (ch == '\r') {
            if (i + 1 < n && s[i + 1] == '\n')
                i++;
            ch = '\n';
        }
        if (ch == '\n' && single_line) {
            ch = ' ';
        }
        au_buf_putc(&t, ch);
    }
    size_t tn = t.len;
    if (max_chars >= 0) {
        size_t have = au_utf8_count(c->value.data ? c->value.data : "", c->value.len) -
                      au_utf8_count(c->value.data ? c->value.data + a : "", b - a);
        size_t room = (size_t)max_chars > have ? (size_t)max_chars - have : 0;
        /* Keep at most room characters, cut on a character boundary. */
        size_t i = 0, k = 0;
        while (i < tn && k < room) {
            i++;
            while (i < tn && is_cont((unsigned char)t.data[i]))
                i++;
            k++;
        }
        tn = i;
    }
    if (tn == 0 && a == b) {
        au_buf_free(&t);
        return false;
    }
    snapshot(c, kind);
    replace(c, a, b, t.data ? t.data : "", tn);
    c->undo_kind = (uint8_t)kind;
    au_buf_free(&t);
    return true;
}

bool au_edit_delete(AuControl *c, int dir, bool by_word)
{
    uint32_t a, b;
    au_edit_range(c, &a, &b);
    const char *s = c->value.data ? c->value.data : "";
    size_t len = c->value.len;
    if (a == b) {
        if (dir < 0)
            a = by_word ? au_edit_prev_word(s, len, a) : au_edit_prev_char(s, len, a);
        else
            b = by_word ? au_edit_next_word(s, len, b) : au_edit_next_char(s, len, b);
    }
    if (a == b)
        return false;
    snapshot(c, AU_EDIT_DELETING);
    replace(c, a, b, "", 0);
    c->undo_kind = AU_EDIT_DELETING;
    return true;
}

bool au_edit_delete_to(AuControl *c, uint32_t to)
{
    if (au_edit_has_selection(c))
        return au_edit_delete(c, -1, false);
    uint32_t a = c->caret, b = to;
    if (b > c->value.len)
        b = (uint32_t)c->value.len;
    if (a > b) {
        uint32_t x = a;
        a = b;
        b = x;
    }
    if (a == b)
        return false;
    snapshot(c, AU_EDIT_OTHER);
    replace(c, a, b, "", 0);
    return true;
}

void au_edit_move_to(AuControl *c, uint32_t off, bool extend)
{
    if (off > c->value.len)
        off = (uint32_t)c->value.len;
    c->caret = off;
    if (!extend)
        c->anchor = off;
    c->undo_kind = AU_EDIT_OTHER; /* a caret move ends a run of typing */
}

void au_edit_move(AuControl *c, int dir, bool by_word, bool extend)
{
    const char *s = c->value.data ? c->value.data : "";
    size_t len = c->value.len;
    if (!extend && au_edit_has_selection(c) && !by_word) {
        uint32_t a, b;
        au_edit_range(c, &a, &b);
        au_edit_move_to(c, dir < 0 ? a : b, false);
        return;
    }
    uint32_t off = c->caret;
    if (dir < 0)
        off = by_word ? au_edit_prev_word(s, len, off) : au_edit_prev_char(s, len, off);
    else
        off = by_word ? au_edit_next_word(s, len, off) : au_edit_next_char(s, len, off);
    au_edit_move_to(c, off, extend);
}

void au_edit_select(AuControl *c, uint32_t anchor, uint32_t caret)
{
    if (anchor > c->value.len)
        anchor = (uint32_t)c->value.len;
    if (caret > c->value.len)
        caret = (uint32_t)c->value.len;
    c->anchor = anchor;
    c->caret = caret;
    c->undo_kind = AU_EDIT_OTHER;
}

void au_edit_select_all(AuControl *c)
{
    au_edit_select(c, 0, (uint32_t)c->value.len);
}

void au_edit_selected_text(const AuControl *c, AuBuf *out)
{
    uint32_t a, b;
    au_edit_range(c, &a, &b);
    if (b > a)
        au_buf_append(out, c->value.data + a, b - a);
}

bool au_edit_undo(AuControl *c)
{
    if (!c->has_undo)
        return false;
    AuBuf cur = c->value;
    uint32_t ca = c->anchor, cc = c->caret;
    c->value = c->undo;
    c->undo = cur;
    c->anchor = c->undo_anchor;
    c->caret = c->undo_caret;
    c->undo_anchor = ca;
    c->undo_caret = cc;
    c->undo_kind = AU_EDIT_OTHER;
    c->dirty = true;
    au_buf_cstr(&c->value);
    return true;
}
