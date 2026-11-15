/* The bookmarks and history list. See listview.h. */
#include "listview.h"

#include "favicon.h"
#include "chrome.h"

#include <stdio.h>
#include <string.h>

void au_listview_init(AuListView *lv)
{
    memset(lv, 0, sizeof *lv);
    lv->selected = -1;
    lv->active = true;
}

void au_listview_clear(AuListView *lv)
{
    for (int i = 0; i < lv->n; i++) {
        au_free(lv->rows[i].text);
        au_free(lv->rows[i].detail);
        au_free(lv->rows[i].icon_host);
    }
    lv->n = 0;
}

void au_listview_free(AuListView *lv)
{
    au_listview_clear(lv);
    au_free(lv->rows);
    memset(lv, 0, sizeof *lv);
}

void au_listview_add(AuListView *lv, AuRowKind kind, int indent, bool expanded, const char *text,
                     const char *detail, uint32_t id)
{
    if (lv->n == lv->cap) {
        lv->cap = lv->cap ? lv->cap * 2 : 32;
        lv->rows = au_realloc(lv->rows, (size_t)lv->cap * sizeof *lv->rows);
    }
    AuListRow *r = &lv->rows[lv->n++];
    r->kind = kind;
    r->indent = indent;
    r->expanded = expanded;
    r->text = au_strndup(text ? text : "", text ? strlen(text) : 0);
    r->detail = au_strndup(detail ? detail : "", detail ? strlen(detail) : 0);
    r->id = id;
    r->icon_host = NULL;
}

/* Notes the host of the last row added when the icon store has an icon
 * for url's site. */
static void row_icon(AuListView *lv, const char *url)
{
    char host[256];
    if (!lv->icons || !lv->n || !url || !au_favicons_host(url, host, sizeof host) ||
        !au_favicons_for_host(lv->icons, host))
        return;
    lv->rows[lv->n - 1].icon_host = au_strndup(host, strlen(host));
}

static void clamp(AuListView *lv)
{
    int max = au_listview_content_height(lv) - lv->height;
    if (max < 0)
        max = 0;
    if (lv->scroll > max)
        lv->scroll = max;
    if (lv->scroll < 0)
        lv->scroll = 0;
    if (lv->selected >= lv->n)
        lv->selected = lv->n - 1;
}

void au_listview_from_bookmarks(AuListView *lv, const AuBookmarks *bm)
{
    uint32_t sel = lv->selected >= 0 && lv->selected < lv->n ? lv->rows[lv->selected].id : 0;
    au_listview_clear(lv);
    lv->selected = -1;
    const AuBookmark *root = &bm->root;
    for (const AuBookmark *b = root->first; b;) {
        if (b->id == sel)
            lv->selected = lv->n;
        au_listview_add(lv, b->folder ? AU_ROW_FOLDER : AU_ROW_LINK, au_bookmark_depth(b), b->expanded, b->title,
                        b->folder ? NULL : b->url, b->id);
        if (!b->folder)
            row_icon(lv, b->url);
        b = au_bookmark_next(b, root, b->folder && !b->expanded);
    }
    clamp(lv);
}

void au_listview_from_history(AuListView *lv, const AuHistory *h, AuTime now)
{
    au_listview_clear(lv);
    int64_t today = au_day_of(now + h->utc_offset);
    int64_t day = 0;
    char label[96], when[16];
    for (size_t i = 0; i < h->n; i++) {
        const AuHistoryEntry *e = &h->e[i];
        AuTime local = e->last_visit + h->utc_offset;
        int64_t d = au_day_of(local);
        if (i == 0 || d != day) {
            day = d;
            au_day_label(d, today, label, sizeof label);
            au_listview_add(lv, AU_ROW_HEADER, 0, true, label, NULL, 0);
        }
        int64_t secs = local - d * 86400;
        snprintf(when, sizeof when, "%02d:%02d", (int)(secs / 3600), (int)(secs / 60 % 60));
        AuBuf detail;
        au_buf_init(&detail);
        au_buf_puts(&detail, when);
        au_buf_puts(&detail, "  ");
        au_buf_puts(&detail, e->url);
        au_listview_add(lv, AU_ROW_LINK, 1, false, *e->title ? e->title : e->url, au_buf_cstr(&detail),
                        (uint32_t)i);
        row_icon(lv, e->url);
        au_buf_free(&detail);
    }
    clamp(lv);
}

int au_listview_content_height(const AuListView *lv)
{
    return lv->n * AU_LIST_ROW_H + 4;
}

int au_listview_hit(const AuListView *lv, int x, int y, bool *disclosure)
{
    if (disclosure)
        *disclosure = false;
    int i = (y + lv->scroll - 2) / AU_LIST_ROW_H;
    if (y + lv->scroll - 2 < 0 || i < 0 || i >= lv->n)
        return -1;
    const AuListRow *r = &lv->rows[i];
    int tx = 6 + r->indent * 16;
    if (disclosure && r->kind == AU_ROW_FOLDER && x >= tx - 2 && x < tx + 14)
        *disclosure = true;
    return i;
}

void au_listview_reveal(AuListView *lv, int i)
{
    if (i < 0 || i >= lv->n)
        return;
    int top = 2 + i * AU_LIST_ROW_H;
    if (top < lv->scroll)
        lv->scroll = top - 2;
    else if (top + AU_LIST_ROW_H > lv->scroll + lv->height)
        lv->scroll = top + AU_LIST_ROW_H - lv->height + 2;
    clamp(lv);
}

static void triangle(AuCanvas *c, float x, float y, bool open, AuColor col)
{
    AuPath p;
    au_path_init(&p);
    if (open) {
        au_path_move_to(&p, x, y - 2);
        au_path_line_to(&p, x + 9, y - 2);
        au_path_line_to(&p, x + 4.5f, y + 3);
    } else {
        au_path_move_to(&p, x + 2, y - 5);
        au_path_line_to(&p, x + 7, y - 0.5f);
        au_path_line_to(&p, x + 2, y + 4);
    }
    au_path_close(&p);
    au_fill_path(c, &p, AU_FILL_NONZERO, col);
    au_path_free(&p);
}

static void folder_icon(AuCanvas *c, float x, float y)
{
    au_fill_rect(c, (AuRect){x, y + 1, 6, 2}, AU_RGB(150, 150, 210));
    au_fill_rect(c, (AuRect){x, y + 3, 13, 9}, AU_RGB(150, 150, 210));
    au_fill_rect(c, (AuRect){x + 1, y + 4, 11, 7}, AU_RGB(204, 204, 255));
}

static void page_icon(AuCanvas *c, float x, float y)
{
    au_fill_rect(c, (AuRect){x, y, 10, 12}, AU_RGB(102, 102, 102));
    au_fill_rect(c, (AuRect){x + 1, y + 1, 8, 10}, AU_RGB(255, 255, 255));
    au_fill_rect(c, (AuRect){x + 2, y + 4, 6, 1}, AU_RGB(170, 170, 170));
    au_fill_rect(c, (AuRect){x + 2, y + 6, 6, 1}, AU_RGB(170, 170, 170));
    au_fill_rect(c, (AuRect){x + 2, y + 8, 4, 1}, AU_RGB(170, 170, 170));
}

void au_listview_paint(const AuListView *lv, AuCanvas *c, AuFontSet *fonts)
{
    AuRect all = {0, 0, (float)lv->width, (float)lv->height};
    au_canvas_clip_push(c, all);
    au_fill_rect(c, all, AU_RGB(255, 255, 255));
    AuFontFace *f = au_chrome_label_face(fonts);
    AuFontFace *small = au_fontset_match(fonts, "Geneva, system-ui", 400, false);
    int first = (lv->scroll - 2) / AU_LIST_ROW_H;
    if (first < 0)
        first = 0;
    char buf[400];
    for (int i = first; i < lv->n; i++) {
        float top = (float)(2 + i * AU_LIST_ROW_H - lv->scroll);
        if (top > (float)lv->height)
            break;
        const AuListRow *r = &lv->rows[i];
        bool sel = i == lv->selected;
        AuColor ink = AU_RGB(0, 0, 0);
        if (r->kind == AU_ROW_HEADER) {
            au_fill_rect(c, (AuRect){0, top, (float)lv->width, AU_LIST_ROW_H}, AU_RGB(221, 221, 221));
            au_fill_rect(c, (AuRect){0, top + AU_LIST_ROW_H - 1, (float)lv->width, 1}, AU_RGB(153, 153, 153));
        }
        if (sel) {
            AuColor hl = lv->active ? AU_RGB(102, 102, 204) : AU_RGB(204, 204, 204);
            au_fill_rect(c, (AuRect){0, top, (float)lv->width, AU_LIST_ROW_H}, hl);
            if (lv->active)
                ink = AU_RGB(255, 255, 255);
        }
        float x = 6 + (float)r->indent * 16;
        float base = top + 13;
        if (r->kind == AU_ROW_FOLDER) {
            triangle(c, x, top + 9, r->expanded, sel && lv->active ? ink : AU_RGB(68, 68, 68));
            folder_icon(c, x + 14, top + 2);
            x += 32;
        } else if (r->kind == AU_ROW_LINK) {
            /* Looked up at paint time: the store may have dropped it. */
            const AuSurface *ic = r->icon_host ? au_favicons_for_host(lv->icons, r->icon_host) : NULL;
            if (ic)
                au_draw_image(c, ic, (AuIRect){0, 0, ic->width, ic->height}, (AuRect){x, top + 1, 16, 16},
                              AU_FILTER_NEAREST, 1.0f);
            else
                page_icon(c, x + 2, top + 3);
            x += 18;
        }
        if (f) {
            float avail = (float)lv->width - x - 8;
            if (*r->detail)
                avail = avail * 0.55f;
            au_chrome_ellipsize(f, 12, r->text, avail, buf, sizeof buf);
            float w = au_draw_text(c, f, 12, x, base, buf, strlen(buf), ink);
            if (*r->detail && small) {
                float dx = x + w + 10;
                au_chrome_ellipsize(small, 10, r->detail, (float)lv->width - dx - 6, buf, sizeof buf);
                au_draw_text(c, small, 10, dx, base, buf, strlen(buf),
                             sel && lv->active ? AU_RGB(230, 230, 255) : AU_RGB(120, 120, 120));
            }
        }
    }
    au_canvas_clip_pop(c);
}
