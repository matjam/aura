/* Bookmarks: a tree of folders and links, stored in the Netscape bookmark
 * file format (NETSCAPE-Bookmark-file-1), which other browsers import and
 * export. Portable C99.
 *
 * The file is written here and read back with Aura's own HTML parser
 * (au_html_parse): the format is HTML whose <DL> lists nest, an <H3> in a
 * <DT> names a folder whose items are in the <DL> that follows it, and an
 * <A HREF> in a <DT> is a bookmark. ADD_DATE attributes hold AuTime
 * values (Aura's times are local; other browsers write UTC, so imported
 * dates can be off by the time zone, which only affects sorting by date).
 *
 * Tree operations never recurse: walk with au_bookmark_next. */
#ifndef AURA_BROWSER_BOOKMARKS_H
#define AURA_BROWSER_BOOKMARKS_H

#include "history.h"

typedef struct AuBookmark AuBookmark;
struct AuBookmark {
    bool folder;
    bool expanded; /* folder open in the manager window (not saved) */
    char *title;
    char *url; /* "" for folders */
    AuTime added;
    uint32_t id; /* unique within the tree, for menus and windows */
    AuBookmark *parent, *first, *last, *prev, *next;
};

typedef struct AuBookmarks {
    AuBookmark root; /* a folder; its title is the file's <H1> */
    uint32_t next_id;
    uint32_t generation; /* bumped on every change */
} AuBookmarks;

void au_bookmarks_init(AuBookmarks *bm);
void au_bookmarks_free(AuBookmarks *bm);
void au_bookmarks_clear(AuBookmarks *bm);

/* Adds a bookmark or a folder at the end of folder (the root when NULL),
 * or before `before` when it is not NULL. */
AuBookmark *au_bookmark_add(AuBookmarks *bm, AuBookmark *folder, AuBookmark *before, const char *title,
                            const char *url, AuTime added);
AuBookmark *au_bookmark_add_folder(AuBookmarks *bm, AuBookmark *folder, AuBookmark *before, const char *title,
                                   AuTime added);
void au_bookmark_rename(AuBookmarks *bm, AuBookmark *b, const char *title);
void au_bookmark_set_url(AuBookmarks *bm, AuBookmark *b, const char *url);
/* Removes b (and, for a folder, everything in it) and frees it. */
void au_bookmark_delete(AuBookmarks *bm, AuBookmark *b);
/* Moves b into folder before `before` (end when NULL). Refuses (returns
 * false) to move a folder into itself or its descendants. */
bool au_bookmark_move(AuBookmarks *bm, AuBookmark *b, AuBookmark *folder, AuBookmark *before);

/* Preorder walk below root: the node after b, or NULL. Pass root to
 * start. skip_children skips b's subtree. */
AuBookmark *au_bookmark_next(const AuBookmark *b, const AuBookmark *root, bool skip_children);
/* Depth below the root (children of the root are 0). */
int au_bookmark_depth(const AuBookmark *b);
AuBookmark *au_bookmark_find_id(AuBookmarks *bm, uint32_t id);
AuBookmark *au_bookmark_find_url(AuBookmarks *bm, const char *url);
size_t au_bookmarks_count(const AuBookmarks *bm); /* links and folders */

/* Best completion for typed text among bookmark URLs (as
 * au_history_complete). NULL when none. */
const AuBookmark *au_bookmarks_complete(const AuBookmarks *bm, const char *typed);

void au_bookmarks_serialize(const AuBookmarks *bm, AuBuf *out);
/* Replaces the tree with the file's contents. Returns false when the
 * data is not a bookmark file (no NETSCAPE-Bookmark-file doctype and no
 * bookmarks found); the tree is then left empty. */
bool au_bookmarks_parse(AuBookmarks *bm, const char *data, size_t len);

#endif
