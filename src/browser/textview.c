/* The fixed-width text view of the developer windows (see textview.h). */
#include "textview.h"

#include <stdio.h>
#include <string.h>

#define TAB 4
#define MAX_RUNS 96
#define FAMILIES "Monaco, monospace"

/* ---- Cells ---- */

/* Steps over one character at s (n > 0 bytes left) standing at cell:
 * returns its bytes and adds its cells. */
static size_t step(const char *s, size_t n, uint32_t *cell)
{
    unsigned char ch = (unsigned char)*s;
    if (ch == '\t') {
        *cell += TAB - *cell % TAB;
        return 1;
    }
    (*cell)++;
    if (ch < 0x80)
        return 1;
    size_t adv = 1;
    au_utf8_decode(s, n, &adv);
    return adv ? adv : 1;
}

uint32_t au_textview_cells(const char *text, size_t len)
{
    uint32_t cell = 0;
    for (size_t i = 0; i < len;)
        i += step(text + i, len - i, &cell);
    return cell;
}

/* The byte offset of the first character that starts at or after cell;
 * *at receives the cell it starts at (the line's cell count at the end). */
static size_t offset_at_cell(const char *text, size_t len, uint32_t cell, uint32_t *at)
{
    uint32_t c = 0;
    size_t i = 0;
    while (i < len && c < cell)
        i += step(text + i, len - i, &c);
    if (at)
        *at = c;
    return i;
}

static uint32_t cell_of_offset(const char *text, size_t len, size_t off)
{
    return au_textview_cells(text, off < len ? off : len);
}

/* ---- Set-up ---- */

void au_textview_init(AuTextView *tv)
{
    memset(tv, 0, sizeof *tv);
    tv->size = 10;
    tv->selected = -1;
    tv->cols = 80;
    tv->row_h = 13;
    tv->visible_rows = 1;
    au_buf_init(&tv->scratch);
}

void au_textview_free(AuTextView *tv)
{
    if (tv->font)
        au_fontlist_free(tv->font);
    au_free(tv->first_row);
    au_buf_free(&tv->scratch);
    memset(tv, 0, sizeof *tv);
}

void au_textview_set_source(AuTextView *tv, const AuTextSource *src)
{
    tv->src = *src;
    tv->has_sel = false;
    tv->selected = -1;
    tv->top = 0;
}

static void get_line(const AuTextView *tv, uint32_t i, AuTextLine *ln)
{
    memset(ln, 0, sizeof *ln);
    ln->text = "";
    if (tv->src.line && i < tv->nlines)
        tv->src.line(tv->src.ctx, i, ln);
}

uint32_t au_textview_max_top(const AuTextView *tv)
{
    return tv->total_rows > (uint32_t)tv->visible_rows ? tv->total_rows - (uint32_t)tv->visible_rows : 0;
}

void au_textview_scroll_to(AuTextView *tv, uint32_t top)
{
    uint32_t max = au_textview_max_top(tv);
    tv->top = top > max ? max : top;
}

bool au_textview_at_end(const AuTextView *tv)
{
    return tv->top >= au_textview_max_top(tv);
}

void au_textview_layout(AuTextView *tv, AuFontSet *fonts, int width, int height)
{
    if (!tv->font && fonts)
        tv->font = au_fontlist_parse(fonts, FAMILIES, 400, false);
    if (tv->size < 6)
        tv->size = 10;
    float asc = tv->size * 0.8f, desc = tv->size * 0.25f;
    tv->cell_w = tv->size * 0.6f;
    if (tv->font) {
        float w = au_text_width_list(tv->font, tv->size, "0", 1);
        if (w > 0.5f)
            tv->cell_w = w;
        AuFontFace *f = au_fontset_match(fonts, FAMILIES, 400, false);
        if (f) {
            AuFontMetrics m;
            au_font_metrics(f, tv->size, &m);
            asc = m.ascent;
            desc = m.descent;
        }
    }
    tv->row_h = (int)(asc + desc + 0.99f) + 2;
    tv->baseline = 1 + (int)(asc + 0.5f);
    tv->width = width;
    tv->height = height;
    tv->nlines = tv->src.count ? tv->src.count(tv->src.ctx) : 0;
    if (tv->numbers) {
        int digits = 1;
        for (uint32_t n = tv->nlines; n >= 10; n /= 10)
            digits++;
        if (digits < 3)
            digits = 3;
        tv->gutter = (int)((float)(digits + 1) * tv->cell_w) + 6;
    } else {
        tv->gutter = 4;
    }
    tv->cols = (int)((float)(width - tv->gutter - 2) / tv->cell_w);
    if (tv->cols < 4)
        tv->cols = 4;
    tv->visible_rows = height / tv->row_h;
    if (tv->visible_rows < 1)
        tv->visible_rows = 1;
    tv->first_row = au_realloc(tv->first_row, ((size_t)tv->nlines + 1) * sizeof *tv->first_row);
    uint32_t row = 0;
    for (uint32_t i = 0; i < tv->nlines; i++) {
        tv->first_row[i] = row;
        uint32_t rows = 1;
        if (tv->wrap) {
            AuTextLine ln;
            get_line(tv, i, &ln);
            uint32_t cells = ln.known_cells ? ln.cells : au_textview_cells(ln.text, ln.len);
            if (cells > (uint32_t)tv->cols)
                rows = (cells + (uint32_t)tv->cols - 1) / (uint32_t)tv->cols;
        }
        row += rows;
    }
    tv->first_row[tv->nlines] = row;
    tv->total_rows = row;
    au_textview_scroll_to(tv, tv->top);
    if (tv->selected >= (int32_t)tv->nlines)
        tv->selected = -1;
    if (tv->has_sel && (tv->anchor.line >= tv->nlines || tv->focus.line >= tv->nlines))
        tv->has_sel = false;
}

/* The line holding row (row < total_rows). */
static uint32_t line_of_row(const AuTextView *tv, uint32_t row)
{
    uint32_t lo = 0, hi = tv->nlines;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (tv->first_row[mid] <= row)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

/* ---- Selection ---- */

static int pos_cmp(AuTvPos a, AuTvPos b)
{
    if (a.line != b.line)
        return a.line < b.line ? -1 : 1;
    return a.off < b.off ? -1 : a.off > b.off;
}

static bool sel_range(const AuTextView *tv, AuTvPos *a, AuTvPos *b)
{
    if (!tv->has_sel || pos_cmp(tv->anchor, tv->focus) == 0)
        return false;
    bool fwd = pos_cmp(tv->anchor, tv->focus) < 0;
    *a = fwd ? tv->anchor : tv->focus;
    *b = fwd ? tv->focus : tv->anchor;
    return true;
}

bool au_textview_has_selection(const AuTextView *tv)
{
    AuTvPos a, b;
    if (tv->select_rows)
        return tv->selected >= 0;
    return sel_range(tv, &a, &b);
}

void au_textview_clear_selection(AuTextView *tv)
{
    tv->has_sel = false;
    tv->selected = -1;
}

void au_textview_select_all(AuTextView *tv)
{
    if (tv->select_rows || !tv->nlines)
        return;
    AuTextLine ln;
    get_line(tv, tv->nlines - 1, &ln);
    tv->anchor = (AuTvPos){0, 0};
    tv->focus = (AuTvPos){tv->nlines - 1, (uint32_t)ln.len};
    tv->has_sel = true;
}

static void append_range(AuTextView *tv, AuTvPos a, AuTvPos b, AuBuf *out)
{
    for (uint32_t i = a.line; i <= b.line && i < tv->nlines; i++) {
        AuTextLine ln;
        get_line(tv, i, &ln);
        size_t from = i == a.line ? a.off : 0, to = i == b.line ? b.off : ln.len;
        if (to > ln.len)
            to = ln.len;
        if (from < to)
            au_buf_append(out, ln.text + from, to - from);
        if (i < b.line) {
            AuTextLine next;
            get_line(tv, i + 1, &next);
            if (!next.joined)
                au_buf_putc(out, '\n');
        }
    }
}

bool au_textview_selection_text(AuTextView *tv, AuBuf *out)
{
    if (tv->select_rows) {
        if (tv->selected < 0)
            return false;
        AuTextLine ln;
        get_line(tv, (uint32_t)tv->selected, &ln);
        au_buf_append(out, ln.text, ln.len);
        return true;
    }
    AuTvPos a, b;
    if (!sel_range(tv, &a, &b))
        return false;
    append_range(tv, a, b, out);
    return true;
}

void au_textview_all_text(AuTextView *tv, AuBuf *out)
{
    if (!tv->nlines)
        return;
    AuTextLine ln;
    get_line(tv, tv->nlines - 1, &ln);
    append_range(tv, (AuTvPos){0, 0}, (AuTvPos){tv->nlines - 1, (uint32_t)ln.len}, out);
    au_buf_putc(out, '\n');
}

void au_textview_reveal(AuTextView *tv, AuTvPos pos)
{
    if (pos.line >= tv->nlines)
        return;
    uint32_t row = tv->first_row[pos.line];
    if (tv->wrap) {
        AuTextLine ln;
        get_line(tv, pos.line, &ln);
        row += cell_of_offset(ln.text, ln.len, pos.off) / (uint32_t)tv->cols;
        if (row >= tv->first_row[pos.line + 1])
            row = tv->first_row[pos.line + 1] - 1;
    }
    if (row >= tv->top && row < tv->top + (uint32_t)tv->visible_rows)
        return;
    uint32_t lead = (uint32_t)tv->visible_rows / 3;
    au_textview_scroll_to(tv, row > lead ? row - lead : 0);
}

/* ---- Hit testing ---- */

/* The row at view y, clamped to the rows there are; false when empty. */
static bool row_at(const AuTextView *tv, int y, uint32_t *row)
{
    if (!tv->total_rows)
        return false;
    long r = (long)tv->top + (y < 0 ? -1 : y / tv->row_h);
    if (r < 0)
        r = 0;
    if ((uint32_t)r >= tv->total_rows)
        r = (long)tv->total_rows - 1;
    *row = (uint32_t)r;
    return true;
}

int32_t au_textview_line_at(const AuTextView *tv, int x, int y)
{
    (void)x;
    if (y < 0 || !tv->total_rows)
        return -1;
    uint32_t row = tv->top + (uint32_t)(y / tv->row_h);
    if (row >= tv->total_rows)
        return -1;
    return (int32_t)line_of_row(tv, row);
}

static bool pos_at(const AuTextView *tv, int x, int y, AuTvPos *pos)
{
    uint32_t row;
    if (!row_at(tv, y, &row))
        return false;
    uint32_t i = line_of_row(tv, row);
    AuTextLine ln;
    get_line(tv, i, &ln);
    uint32_t sub = row - tv->first_row[i];
    uint32_t start = tv->wrap ? sub * (uint32_t)tv->cols : 0;
    float fx = ((float)(x - tv->gutter)) / tv->cell_w + 0.5f;
    uint32_t dc = fx <= 0 ? 0 : (uint32_t)fx;
    if (dc > (uint32_t)tv->cols)
        dc = (uint32_t)tv->cols;
    pos->line = i;
    pos->off = (uint32_t)offset_at_cell(ln.text, ln.len, start + dc, NULL);
    return true;
}

static bool word_char(unsigned char c)
{
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* The word (mode 2) or line (mode 3) around pos. */
static void unit_at(const AuTextView *tv, AuTvPos pos, int mode, AuTvPos *a, AuTvPos *b)
{
    AuTextLine ln;
    get_line(tv, pos.line, &ln);
    size_t s = pos.off < ln.len ? pos.off : ln.len, e = s;
    if (mode == 3) {
        s = 0;
        e = ln.len;
    } else if (s < ln.len && word_char((unsigned char)ln.text[s])) {
        while (s > 0 && word_char((unsigned char)ln.text[s - 1]))
            s--;
        while (e < ln.len && word_char((unsigned char)ln.text[e]))
            e++;
    } else if (s < ln.len) {
        e = s + 1;
        while (e < ln.len && ((unsigned char)ln.text[e] & 0xC0) == 0x80)
            e++;
    }
    *a = (AuTvPos){pos.line, (uint32_t)s};
    *b = (AuTvPos){pos.line, (uint32_t)e};
}

bool au_textview_mouse_down(AuTextView *tv, int x, int y, int clicks, bool shift)
{
    if (tv->select_rows) {
        int32_t line = au_textview_line_at(tv, x, y);
        bool changed = line != tv->selected;
        tv->selected = line;
        return changed;
    }
    AuTvPos p;
    if (!pos_at(tv, x, y, &p)) {
        tv->has_sel = false;
        return true;
    }
    if (clicks >= 2) {
        tv->drag_mode = clicks >= 3 ? 3 : 2;
        unit_at(tv, p, tv->drag_mode, &tv->drag_a, &tv->drag_b);
        tv->anchor = tv->drag_a;
        tv->focus = tv->drag_b;
        tv->has_sel = true;
        return true;
    }
    tv->drag_mode = 1;
    if (shift && tv->has_sel) {
        tv->focus = p;
    } else {
        tv->anchor = tv->focus = p;
        tv->has_sel = true; /* empty until the mouse moves */
    }
    return true;
}

bool au_textview_mouse_drag(AuTextView *tv, int x, int y)
{
    if (tv->select_rows || !tv->drag_mode)
        return false;
    bool scrolled = false;
    if (y < 0 && tv->top > 0) {
        tv->top--;
        scrolled = true;
    } else if (y >= tv->height && tv->top < au_textview_max_top(tv)) {
        tv->top++;
        scrolled = true;
    }
    AuTvPos p;
    if (!pos_at(tv, x, y, &p))
        return scrolled;
    AuTvPos old_a = tv->anchor, old_f = tv->focus;
    if (tv->drag_mode == 1) {
        tv->focus = p;
    } else {
        AuTvPos a, b;
        unit_at(tv, p, tv->drag_mode, &a, &b);
        if (pos_cmp(a, tv->drag_a) < 0) {
            tv->anchor = tv->drag_b;
            tv->focus = a;
        } else {
            tv->anchor = tv->drag_a;
            tv->focus = pos_cmp(b, tv->drag_b) > 0 ? b : tv->drag_b;
        }
    }
    return scrolled || pos_cmp(old_a, tv->anchor) != 0 || pos_cmp(old_f, tv->focus) != 0;
}

void au_textview_mouse_up(AuTextView *tv)
{
    tv->drag_mode = 0;
}

bool au_textview_key(AuTextView *tv, int key)
{
    uint32_t page = tv->visible_rows > 1 ? (uint32_t)tv->visible_rows - 1 : 1;
    switch (key) {
    case AU_KEY_UP:
    case AU_KEY_DOWN:
        if (tv->select_rows && tv->nlines) {
            int32_t s = tv->selected + (key == AU_KEY_UP ? -1 : 1);
            if (s < 0)
                s = 0;
            if (s >= (int32_t)tv->nlines)
                s = (int32_t)tv->nlines - 1;
            tv->selected = s;
            au_textview_reveal(tv, (AuTvPos){(uint32_t)s, 0});
        } else if (key == AU_KEY_UP) {
            au_textview_scroll_to(tv, tv->top ? tv->top - 1 : 0);
        } else {
            au_textview_scroll_to(tv, tv->top + 1);
        }
        return true;
    case AU_KEY_PAGE_UP: au_textview_scroll_to(tv, tv->top > page ? tv->top - page : 0); return true;
    case AU_KEY_PAGE_DOWN: au_textview_scroll_to(tv, tv->top + page); return true;
    case AU_KEY_HOME: au_textview_scroll_to(tv, 0); return true;
    case AU_KEY_END: au_textview_scroll_to(tv, au_textview_max_top(tv)); return true;
    default: return false;
    }
}

/* ---- Find ---- */

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* The first match of needle (lower case, n bytes) in text at or after
 * from, or the last one ending at or before limit when backward; -1 for
 * none. */
static long find_in(const char *text, size_t len, const char *needle, size_t n, size_t from, size_t limit,
                    bool backward)
{
    if (!n || len < n)
        return -1;
    long best = -1;
    for (size_t i = from; i + n <= len && i + n <= limit; i++) {
        size_t k = 0;
        while (k < n && lower(text[i + k]) == needle[k])
            k++;
        if (k == n) {
            if (!backward)
                return (long)i;
            best = (long)i;
        }
    }
    return best;
}

bool au_textview_find(AuTextView *tv, const char *text, bool forward)
{
    size_t n = strlen(text);
    if (!n || !tv->nlines)
        return false;
    char *needle = au_strndup(text, n);
    for (size_t i = 0; i < n; i++)
        needle[i] = lower(needle[i]);
    /* Where to start: after the selection going forward, before it going
     * back; the top of the text without one. */
    AuTvPos a = {0, 0}, b = {0, 0};
    bool have = false;
    if (tv->select_rows && tv->selected >= 0) {
        a = (AuTvPos){(uint32_t)tv->selected, 0};
        b = (AuTvPos){(uint32_t)tv->selected, UINT32_MAX};
        have = true;
    } else if (!tv->select_rows) {
        have = sel_range(tv, &a, &b);
    }
    bool found = false;
    uint32_t line = have ? (forward ? b.line : a.line) : (forward ? 0 : tv->nlines - 1);
    for (uint32_t pass = 0; pass <= tv->nlines && !found; pass++) {
        AuTextLine ln;
        get_line(tv, line, &ln);
        size_t from = 0, limit = ln.len;
        if (pass == 0 && have) {
            if (forward)
                from = b.off;
            else
                limit = a.off;
        }
        long at = from <= ln.len ? find_in(ln.text, ln.len, needle, n, from, limit, !forward) : -1;
        if (at >= 0) {
            found = true;
            if (tv->select_rows) {
                tv->selected = (int32_t)line;
            } else {
                tv->anchor = (AuTvPos){line, (uint32_t)at};
                tv->focus = (AuTvPos){line, (uint32_t)((size_t)at + n)};
                tv->has_sel = true;
            }
            au_textview_reveal(tv, (AuTvPos){line, (uint32_t)at});
            break;
        }
        if (forward)
            line = line + 1 < tv->nlines ? line + 1 : 0;
        else
            line = line ? line - 1 : tv->nlines - 1;
    }
    au_free(needle);
    return found;
}

/* ---- Painting ---- */

static AuColor style_ink(int style)
{
    switch (style) {
    case AU_TS_TAG: return AU_RGB(136, 18, 128);
    case AU_TS_ATTR: return AU_RGB(153, 69, 0);
    case AU_TS_STRING: return AU_RGB(26, 26, 166);
    case AU_TS_COMMENT: return AU_RGB(35, 110, 37);
    case AU_TS_DOCTYPE: return AU_RGB(110, 110, 110);
    case AU_TS_DIM: return AU_RGB(110, 110, 110);
    case AU_TS_ERROR: return AU_RGB(190, 0, 0);
    case AU_TS_WARN: return AU_RGB(128, 88, 0);
    case AU_TS_INPUT: return AU_RGB(0, 51, 170);
    case AU_TS_RESULT: return AU_RGB(70, 70, 70);
    default: return AU_RGB(0, 0, 0);
    }
}

static bool style_band(int style, AuColor *bg)
{
    switch (style) {
    case AU_TS_HEADING: *bg = AU_RGB(221, 221, 221); return true;
    case AU_TS_ERROR: *bg = AU_RGB(255, 240, 240); return true;
    case AU_TS_WARN: *bg = AU_RGB(255, 251, 230); return true;
    default: return false;
    }
}

/* Draws bytes [from, to) of a line whose first byte stands at cell
 * (*cell is advanced), tabs expanded, at pixel x0 for cell row_start. */
static void draw_piece(AuTextView *tv, AuCanvas *c, const char *text, size_t from, size_t to, uint32_t *cell,
                       uint32_t row_start, float x0, float y, AuColor ink)
{
    AuBuf *b = &tv->scratch;
    au_buf_clear(b);
    float x = x0 + (float)(*cell - row_start) * tv->cell_w;
    bool any = false;
    for (size_t i = from; i < to;) {
        unsigned char ch = (unsigned char)text[i];
        uint32_t before = *cell;
        size_t adv = step(text + i, to - i, cell);
        if (ch == '\t') {
            for (uint32_t k = before; k < *cell; k++)
                au_buf_putc(b, ' ');
        } else if (ch < 0x20 || ch == 0x7F) {
            au_buf_putc(b, ' ');
        } else if (ch >= 0x80 && adv == 1) {
            au_buf_putc(b, '?'); /* a byte that is not UTF-8 */
            any = true;
        } else {
            au_buf_append(b, text + i, adv);
            any |= ch != ' ';
        }
        i += adv;
    }
    if (any && tv->font)
        au_draw_text_list(c, tv->font, tv->size, x, y, b->data, b->len, ink);
}

void au_textview_paint(AuTextView *tv, AuCanvas *c, int x, int y)
{
    AuRect all = {(float)x, (float)y, (float)tv->width, (float)tv->height};
    au_canvas_clip_push(c, all);
    au_fill_rect(c, all, AU_RGB(255, 255, 255));
    if (tv->numbers) {
        au_fill_rect(c, (AuRect){(float)x, (float)y, (float)(tv->gutter - 4), (float)tv->height},
                     AU_RGB(240, 240, 240));
        au_fill_rect(c, (AuRect){(float)(x + tv->gutter - 4), (float)y, 1, (float)tv->height},
                     AU_RGB(204, 204, 204));
    }
    AuColor hl = tv->active ? (tv->highlight ? tv->highlight : AU_RGB(179, 215, 255)) : AU_RGB(212, 212, 212);
    AuTvPos sa = {0, 0}, sb = {0, 0};
    bool sel = !tv->select_rows && sel_range(tv, &sa, &sb);
    float tx = (float)(x + tv->gutter);
    uint32_t row = tv->top;
    uint32_t i = row < tv->total_rows ? line_of_row(tv, row) : tv->nlines;
    AuTextLine ln;
    uint32_t loaded = UINT32_MAX;
    memset(&ln, 0, sizeof ln);
    for (int ry = 0; ry < tv->height && row < tv->total_rows; row++, ry += tv->row_h) {
        while (i + 1 <= tv->nlines && tv->first_row[i + 1] <= row)
            i++;
        if (loaded != i) {
            get_line(tv, i, &ln);
            loaded = i;
        }
        uint32_t sub = row - tv->first_row[i];
        /* The row holds the characters that start in its cells. */
        uint32_t start = tv->wrap ? sub * (uint32_t)tv->cols : 0, at = 0;
        size_t from = offset_at_cell(ln.text, ln.len, start, &at);
        size_t to = from;
        for (uint32_t cc = at; to < ln.len && cc < start + (uint32_t)tv->cols;)
            to += step(ln.text + to, ln.len - to, &cc);
        float top = (float)(y + ry);
        AuColor band;
        if (style_band(ln.style, &band))
            au_fill_rect(c, (AuRect){tx - 3, top, (float)tv->width, (float)tv->row_h}, band);
        if (tv->select_rows && (int32_t)i == tv->selected)
            au_fill_rect(c, (AuRect){(float)x, top, (float)tv->width, (float)tv->row_h}, hl);
        if (sel && i >= sa.line && i <= sb.line) {
            size_t s0 = i == sa.line ? sa.off : 0, s1 = i == sb.line ? sb.off : ln.len + 1;
            if (s0 < from)
                s0 = from;
            bool to_edge = s1 > to; /* the selection goes on past this row */
            if (s1 > to)
                s1 = to;
            if (s0 < s1 || (to_edge && s0 <= to)) {
                uint32_t c0 = cell_of_offset(ln.text, ln.len, s0), c1 = cell_of_offset(ln.text, ln.len, s1);
                float hx = tx + (float)(c0 - start) * tv->cell_w;
                float hw = to_edge ? (float)(x + tv->width) - hx : (float)(c1 - c0) * tv->cell_w;
                if (c0 >= start && hw > 0)
                    au_fill_rect(c, (AuRect){hx, top, hw, (float)tv->row_h}, hl);
            }
        }
        float base = top + (float)tv->baseline;
        if (tv->numbers && sub == 0 && ln.number && tv->font) {
            char num[16];
            int nn = snprintf(num, sizeof num, "%lu", (unsigned long)ln.number);
            float nw = (float)nn * tv->cell_w;
            au_draw_text_list(c, tv->font, tv->size, (float)(x + tv->gutter - 8) - nw, base, num, (size_t)nn,
                              AU_RGB(128, 128, 128));
        }
        if (from >= to)
            continue;
        AuTextRun runs[MAX_RUNS];
        int nr = tv->src.runs ? tv->src.runs(tv->src.ctx, i, from, to, runs, MAX_RUNS) : 0;
        if (nr <= 0) {
            runs[0].start = (uint32_t)from;
            runs[0].style = ln.style;
            nr = 1;
        }
        uint32_t cell = at;
        for (int k = 0; k < nr; k++) {
            size_t r0 = runs[k].start < from ? from : runs[k].start;
            size_t r1 = k + 1 < nr ? runs[k + 1].start : to;
            if (r1 > to)
                r1 = to;
            if (r0 < r1)
                draw_piece(tv, c, ln.text, r0, r1, &cell, start, tx, base, style_ink(runs[k].style));
        }
    }
    au_canvas_clip_pop(c);
}

/* ---- AuTextList ---- */

void au_textlist_init(AuTextList *l)
{
    memset(l, 0, sizeof *l);
}

void au_textlist_clear(AuTextList *l)
{
    for (uint32_t i = 0; i < l->n; i++)
        au_free(l->text[i]);
    l->n = 0;
}

void au_textlist_free(AuTextList *l)
{
    au_textlist_clear(l);
    au_free(l->text);
    au_free(l->style);
    memset(l, 0, sizeof *l);
}

void au_textlist_add(AuTextList *l, int style, const char *text, size_t len)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->text = au_realloc(l->text, l->cap * sizeof *l->text);
        l->style = au_realloc(l->style, l->cap * sizeof *l->style);
    }
    l->text[l->n] = au_strndup(text, len);
    l->style[l->n] = (uint8_t)style;
    l->n++;
}

void au_textlist_add_text(AuTextList *l, int style, const char *text)
{
    const char *s = text;
    while (*s) {
        size_t n = strcspn(s, "\n");
        size_t m = n && s[n - 1] == '\r' ? n - 1 : n;
        au_textlist_add(l, style, s, m);
        s += n;
        if (*s == '\n')
            s++;
    }
}

static uint32_t list_count(void *ctx)
{
    return ((AuTextList *)ctx)->n;
}

static void list_line(void *ctx, uint32_t i, AuTextLine *out)
{
    AuTextList *l = ctx;
    out->text = l->text[i];
    out->len = strlen(l->text[i]);
    out->style = l->style[i];
}

void au_textlist_source(AuTextList *l, AuTextSource *out)
{
    memset(out, 0, sizeof *out);
    out->ctx = l;
    out->count = list_count;
    out->line = list_line;
}
