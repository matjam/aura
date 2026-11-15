/* The document view's state, shared by docview.c and forms.c (internal
 * to src/page). */
#ifndef AURA_PAGE_DOCVIEW_INT_H
#define AURA_PAGE_DOCVIEW_INT_H

#include "page.h"
#include "docimages.h"
#include "script.h"
#include "webfont.h"

/* A style sheet source: a <style>, a <link> or an @import. It is parsed
 * once, when its text is there; the parsed sheet stays until the source
 * goes. A source whose media query does not match the device is neither
 * fetched nor parsed until it matches (deferred). */
typedef struct SheetSrc SheetSrc;
struct SheetSrc {
    char *url;    /* base URL for the sheet (absolute) */
    char *media;  /* media attribute or @import media, NULL for all */
    char *text;   /* a <style>'s text, kept to notice edits; a fetched sheet's text is freed once parsed */
    size_t len;   /* the text's length, also after the text is freed */
    uint32_t req; /* outstanding request, 0 if none */
    bool failed;
    bool deferred;     /* media does not match: not fetched (a <style>: not parsed) yet */
    AuCssSheet *sheet; /* parsed, or NULL; owned here, an import's by its parent's sheet */
    SheetSrc *parent;  /* the importing sheet, NULL for a <style> or <link> */
    uint32_t index;    /* which @import of the parent */
    uint32_t depth;    /* imports above this one */
    SheetSrc **imports; /* one per @import of the sheet, in order */
    uint32_t nimports;
};

/* A <style> or <link> element in document order. */
typedef struct SheetSlot {
    AuElement *el; /* referenced */
    SheetSrc *src;
} SheetSlot;

typedef struct Request {
    uint32_t id;
    AuResourceKind kind;
    SheetSrc *sheet; /* for style sheets */
    AuImageEntry *image; /* for images: bytes go straight to the image cache */
    bool font;           /* an @font-face source */
    AuBuf data;
    bool background; /* script fetch: does not hold the load event */
} Request;

struct AuDocView {
    AuFontSet *fonts;
    AuWebFonts *webfonts; /* the document's @font-face faces, over fonts */
    AuLoader loader;
    AuDocViewCallbacks cb;
    AuHtmlParser *parser;
    AuDocument *doc;
    char *url, *base;
    char *title;
    char *user_css; /* the user style sheet, or NULL */
    AuCssFontPrefs font_prefs; /* au_docview_set_font_prefs */
    bool has_font_prefs;
    AuCssResolver *resolver;
    AuLayout *layout;
    SheetSlot *slots;
    uint32_t nslots, slots_cap;
    AuCssSheet *user_sheet; /* user_css parsed for this document, or NULL */
    uint32_t ndeferred;     /* sheet sources waiting for their media query to match */
    AuDocImages *imgs; /* docimages.c */
    Request *reqs;
    uint32_t nreqs, reqs_cap;
    int vw, vh;
    int scroll_x, scroll_y;
    bool parsing;
    bool tree_dirty, sheets_dirty, restyle_needed, style_all, images_dirty;
    bool viewport_restyle; /* the viewport changed: restyle, keeping the box tree where styles allow */
    bool finished_reported;
    float last_w, last_h;
    AuElement *hover, *active;
    char hover_link[2048];
    uint32_t scanned_nodes;
    size_t hidden_image_budget; /* the image budget to return to when shown again (au_docview_set_hidden); 0 while shown */
    /* scripts (script.c) */
    AuDocScript *script; /* this document's, while scripting is on */
    bool scripting;      /* the setting, applied at begin */
    bool sheets_rescan;  /* script changed the tree: look for sheets again */
    bool layout_ran;     /* layout changed since observers last ran */
    uint32_t nbg;        /* background requests among reqs */
    double clock;        /* the fallback clock */
    /* forms (forms.c) */
    AuElement *focus;    /* the focused element (referenced), or NULL */
    bool focus_visible;  /* the focus came from the keyboard */
    bool caret_on;       /* the caret is shown (blink phase) */
    uint32_t blink_ms;   /* when the caret last changed phase */
    AuElement *drag;     /* a text field, list box or slider being dragged (referenced), or NULL */
    int drag_clicks;     /* 1 characters, 2 words, 3 lines */
    uint32_t drag_a, drag_b; /* the word or line the drag started on */
    float goal_x;        /* textarea: the x kept by up and down (-1: none) */
    AuElement *pressed;  /* a button held down (referenced), or NULL */
    bool submitting;     /* a submission is being made (no nesting) */
};


/* forms.c */
/* Forgets the focus and drag state (the document is going). */
void au_forms_clear(AuDocView *dv);
void au_forms_autofocus(AuDocView *dv);
/* Mouse handling for form controls; returns true when the event was
 * used by a control (no link or selection then). Called before the
 * view's own handling, after script mouse events. */
bool au_forms_mouse(AuDocView *dv, const AuEvent *ev, const AuHitResult *r);
/* The activation behaviour of a click on target without script (with
 * script, the bindings run it). */
void au_forms_activate(AuDocView *dv, AuElement *target, int x, int y);
void au_dv_invalidate_element(AuDocView *dv, AuElement *el);

#endif
