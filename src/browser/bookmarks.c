/* Bookmarks. See bookmarks.h. */
#include "bookmarks.h"
#include "url.h"

#include "../html/html.h"

#include <stdlib.h>
#include <string.h>

void au_bookmarks_init(AuBookmarks *bm)
{
    memset(bm, 0, sizeof *bm);
    bm->root.folder = true;
    bm->root.expanded = true;
    bm->root.title = au_strndup("Bookmarks", 9);
    bm->root.url = au_strndup("", 0);
    bm->next_id = 1;
}

static void free_node(AuBookmark *b)
{
    au_free(b->title);
    au_free(b->url);
    au_free(b);
}

/* Frees every node below root, without recursion: repeatedly detach the
 * first child; a folder's children are spliced in front of its next
 * sibling first. */
static void free_children(AuBookmark *root)
{
    AuBookmark *b = root->first;
    while (b) {
        if (b->first) {
            /* Hoist b's children in place of b's next. */
            AuBookmark *f = b->first, *l = b->last;
            l->next = b->next;
            if (b->next)
                b->next->prev = l;
            b->next = f;
            f->prev = b;
            b->first = b->last = NULL;
        }
        AuBookmark *n = b->next;
        free_node(b);
        b = n;
    }
    root->first = root->last = NULL;
}

void au_bookmarks_clear(AuBookmarks *bm)
{
    free_children(&bm->root);
    bm->generation++;
}

void au_bookmarks_free(AuBookmarks *bm)
{
    free_children(&bm->root);
    au_free(bm->root.title);
    au_free(bm->root.url);
    memset(bm, 0, sizeof *bm);
}

static void unlink_node(AuBookmark *b)
{
    AuBookmark *p = b->parent;
    if (!p)
        return;
    if (b->prev)
        b->prev->next = b->next;
    else
        p->first = b->next;
    if (b->next)
        b->next->prev = b->prev;
    else
        p->last = b->prev;
    b->parent = b->prev = b->next = NULL;
}

static void link_node(AuBookmark *folder, AuBookmark *b, AuBookmark *before)
{
    if (before && before->parent != folder)
        before = NULL;
    b->parent = folder;
    if (before) {
        b->next = before;
        b->prev = before->prev;
        if (before->prev)
            before->prev->next = b;
        else
            folder->first = b;
        before->prev = b;
    } else {
        b->prev = folder->last;
        b->next = NULL;
        if (folder->last)
            folder->last->next = b;
        else
            folder->first = b;
        folder->last = b;
    }
}

static char *dup_clean(const char *s)
{
    if (!s)
        s = "";
    char *c = au_strndup(s, strlen(s));
    for (char *p = c; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    return c;
}

static AuBookmark *add(AuBookmarks *bm, AuBookmark *folder, AuBookmark *before, bool is_folder, const char *title,
                       const char *url, AuTime added)
{
    if (!folder || !folder->folder)
        folder = &bm->root;
    AuBookmark *b = au_calloc(sizeof *b);
    b->folder = is_folder;
    b->title = dup_clean(title);
    b->url = dup_clean(is_folder ? "" : url);
    b->added = added;
    b->id = bm->next_id++;
    link_node(folder, b, before);
    bm->generation++;
    return b;
}

AuBookmark *au_bookmark_add(AuBookmarks *bm, AuBookmark *folder, AuBookmark *before, const char *title,
                            const char *url, AuTime added)
{
    return add(bm, folder, before, false, title, url, added);
}

AuBookmark *au_bookmark_add_folder(AuBookmarks *bm, AuBookmark *folder, AuBookmark *before, const char *title,
                                   AuTime added)
{
    return add(bm, folder, before, true, title, NULL, added);
}

void au_bookmark_rename(AuBookmarks *bm, AuBookmark *b, const char *title)
{
    char *t = dup_clean(title);
    au_free(b->title);
    b->title = t;
    bm->generation++;
}

void au_bookmark_set_url(AuBookmarks *bm, AuBookmark *b, const char *url)
{
    if (b->folder)
        return;
    char *u = dup_clean(url);
    au_free(b->url);
    b->url = u;
    bm->generation++;
}

void au_bookmark_delete(AuBookmarks *bm, AuBookmark *b)
{
    if (b == &bm->root)
        return;
    unlink_node(b);
    free_children(b);
    free_node(b);
    bm->generation++;
}

bool au_bookmark_move(AuBookmarks *bm, AuBookmark *b, AuBookmark *folder, AuBookmark *before)
{
    if (!folder)
        folder = &bm->root;
    if (b == &bm->root || !folder->folder || before == b)
        return false;
    for (AuBookmark *p = folder; p; p = p->parent)
        if (p == b)
            return false;
    unlink_node(b);
    link_node(folder, b, before);
    bm->generation++;
    return true;
}

AuBookmark *au_bookmark_next(const AuBookmark *b, const AuBookmark *root, bool skip_children)
{
    if (!skip_children && b->first)
        return b->first;
    while (b && b != root) {
        if (b->next)
            return b->next;
        b = b->parent;
    }
    return NULL;
}

int au_bookmark_depth(const AuBookmark *b)
{
    int d = -1;
    for (const AuBookmark *p = b->parent; p; p = p->parent)
        d++;
    return d;
}

AuBookmark *au_bookmark_find_id(AuBookmarks *bm, uint32_t id)
{
    for (AuBookmark *b = au_bookmark_next(&bm->root, &bm->root, false); b; b = au_bookmark_next(b, &bm->root, false))
        if (b->id == id)
            return b;
    return NULL;
}

AuBookmark *au_bookmark_find_url(AuBookmarks *bm, const char *url)
{
    for (AuBookmark *b = au_bookmark_next(&bm->root, &bm->root, false); b; b = au_bookmark_next(b, &bm->root, false))
        if (!b->folder && !strcmp(b->url, url))
            return b;
    return NULL;
}

size_t au_bookmarks_count(const AuBookmarks *bm)
{
    size_t n = 0;
    for (AuBookmark *b = au_bookmark_next(&bm->root, &bm->root, false); b; b = au_bookmark_next(b, &bm->root, false))
        n++;
    return n;
}

const AuBookmark *au_bookmarks_complete(const AuBookmarks *bm, const char *typed)
{
    size_t tn = strlen(typed);
    if (tn == 0)
        return NULL;
    bool whole = au_url_scheme_len(typed, tn) != 0;
    const char *t = typed;
    if (!whole && tn > 4 && !strncmp(t, "www.", 4))
        t += 4, tn -= 4;
    for (AuBookmark *b = au_bookmark_next(&bm->root, &bm->root, false); b; b = au_bookmark_next(b, &bm->root, false)) {
        if (b->folder)
            continue;
        size_t kn;
        const char *k = whole ? b->url : au_url_match_key(b->url, &kn);
        if (whole)
            kn = strlen(k);
        if (kn > tn && au_ieq(k, tn, t, tn))
            return b;
    }
    return NULL;
}

/* ---- Writing ---- */

static void put_escaped(AuBuf *out, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': au_buf_puts(out, "&amp;"); break;
        case '<': au_buf_puts(out, "&lt;"); break;
        case '>': au_buf_puts(out, "&gt;"); break;
        case '"': au_buf_puts(out, "&quot;"); break;
        default: au_buf_putc(out, *s); break;
        }
    }
}

static void indent(AuBuf *out, int depth)
{
    for (int i = 0; i <= depth; i++)
        au_buf_puts(out, "    ");
}

void au_bookmarks_serialize(const AuBookmarks *bm, AuBuf *out)
{
    au_buf_puts(out, "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
                     "<!-- This is an automatically generated file.\n"
                     "     It will be read and overwritten.\n"
                     "     DO NOT EDIT! -->\n"
                     "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
                     "<TITLE>Bookmarks</TITLE>\n<H1>");
    put_escaped(out, bm->root.title);
    au_buf_puts(out, "</H1>\n\n<DL><p>\n");
    const AuBookmark *root = &bm->root;
    const AuBookmark *b = root->first;
    int depth = 0;
    while (b) {
        indent(out, depth);
        if (b->folder) {
            au_buf_puts(out, "<DT><H3 ADD_DATE=\"");
            au_put_i64(out, b->added);
            au_buf_puts(out, "\">");
            put_escaped(out, b->title);
            au_buf_puts(out, "</H3>\n");
            indent(out, depth);
            au_buf_puts(out, "<DL><p>\n");
            if (b->first) {
                b = b->first;
                depth++;
                continue;
            }
            indent(out, depth);
            au_buf_puts(out, "</DL><p>\n");
        } else {
            au_buf_puts(out, "<DT><A HREF=\"");
            put_escaped(out, b->url);
            au_buf_puts(out, "\" ADD_DATE=\"");
            au_put_i64(out, b->added);
            au_buf_puts(out, "\">");
            put_escaped(out, b->title);
            au_buf_puts(out, "</A>\n");
        }
        /* Next: the sibling, else close folders on the way up. */
        while (b && !b->next) {
            b = b->parent;
            if (b == root || !b) {
                b = NULL;
                break;
            }
            depth--;
            indent(out, depth);
            au_buf_puts(out, "</DL><p>\n");
        }
        if (b)
            b = b->next;
    }
    au_buf_puts(out, "</DL><p>\n");
}

/* ---- Reading ---- */

static void text_of(AuNode *n, AuBuf *out)
{
    for (AuNode *c = n->first_child; c; c = au_node_next(c, n))
        if (c->type == AU_NODE_TEXT)
            au_buf_puts(out, au_as_text(c)->data);
}

static AuNode *ancestor_dl(AuNode *n)
{
    for (AuNode *p = n->parent; p; p = p->parent)
        if (au_is_html_element(p, AU_ATOM_dl))
            return p;
    return NULL;
}

typedef struct DlMap {
    AuNode *dl;
    AuBookmark *folder;
} DlMap;

static AuTime add_date(AuElement *el, AuAtom attr)
{
    const char *v = au_element_get_attr(el, attr);
    if (!v)
        return 0;
    char *end;
    long long t = strtoll(v, &end, 10);
    return *end ? 0 : (AuTime)t;
}

static AuBookmark *folder_for(DlMap *map, size_t nmap, AuNode *dl, AuBookmark *root)
{
    for (size_t i = 0; i < nmap; i++)
        if (map[i].dl == dl)
            return map[i].folder;
    return root;
}

bool au_bookmarks_parse(AuBookmarks *bm, const char *data, size_t len)
{
    au_bookmarks_clear(bm);
    AuHtmlOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.encoding = AU_HTML_ENC_UTF8;
    AuDocument *doc = au_html_parse(data, len, &opts);
    AuNode *root = &doc->node;
    bool doctype = false;
    AuAtom a_href = au_atom_cstr("href"), a_add = au_atom_cstr("add_date");
    DlMap *map = NULL;
    size_t nmap = 0, mapcap = 0;
    AuBuf t;
    au_buf_init(&t);
    for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
        if (n->type == AU_NODE_DOCTYPE) {
            const char *name = ((AuDoctype *)n)->name;
            if (au_ieq(name, strlen(name), "netscape-bookmark-file-1", 24))
                doctype = true;
            continue;
        }
        if (!au_is_element(n))
            continue;
        AuElement *el = au_as_element(n);
        if (el->ns != AU_NS_HTML)
            continue;
        if (el->tag == AU_ATOM_h1 && !ancestor_dl(n)) {
            au_buf_clear(&t);
            text_of(n, &t);
            if (t.len)
                au_bookmark_rename(bm, &bm->root, au_buf_cstr(&t));
        } else if (el->tag == AU_ATOM_h3) {
            AuNode *dl = ancestor_dl(n);
            if (!dl)
                continue;
            AuBookmark *parent = folder_for(map, nmap, dl, &bm->root);
            au_buf_clear(&t);
            text_of(n, &t);
            AuBookmark *f = au_bookmark_add_folder(bm, parent, NULL, au_buf_cstr(&t), add_date(el, a_add));
            /* The folder's items are in the next <DL>: usually the H3's
             * next sibling, but a <DD> description in between puts it
             * inside the DD. Stop at the next folder or link. */
            AuNode *s = au_node_next_skip(n, root);
            while (s && !au_is_html_element(s, AU_ATOM_dl) && !au_is_html_element(s, AU_ATOM_h3) &&
                   !au_is_html_element(s, AU_ATOM_a))
                s = au_node_next(s, root);
            if (s && au_is_html_element(s, AU_ATOM_dl)) {
                if (nmap == mapcap) {
                    mapcap = mapcap ? mapcap * 2 : 16;
                    map = au_realloc(map, mapcap * sizeof *map);
                }
                map[nmap].dl = s;
                map[nmap].folder = f;
                nmap++;
            }
        } else if (el->tag == AU_ATOM_a) {
            const char *href = au_element_get_attr(el, a_href);
            AuNode *dl = ancestor_dl(n);
            if (!href || !dl)
                continue;
            au_buf_clear(&t);
            text_of(n, &t);
            au_bookmark_add(bm, folder_for(map, nmap, dl, &bm->root), NULL, t.len ? au_buf_cstr(&t) : href, href,
                            add_date(el, a_add));
        }
    }
    au_buf_free(&t);
    au_free(map);
    au_node_unref(root);
    bool ok = doctype || bm->root.first != NULL;
    if (!ok)
        au_bookmarks_clear(bm);
    bm->generation++;
    return ok;
}
