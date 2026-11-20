/* The models of the developer windows: Page Source, Console, Network,
 * Performance, System Status and Report a Problem. Portable C99.
 *
 * A port shows each window with its own widgets and calls these
 * functions; it holds no developer-window logic of its own. A model
 * (AuDevWin) holds:
 *
 * - what the window shows: the text views (AuTextView, textview.h) of
 *   the main text and of the Network window's detail pane, filled from
 *   the page through devtools.c, netlog.c, sourceview.c, sysinfo.c and
 *   bugreport.c, the window's title, the Network column titles and the
 *   one line of status text under the text (the footer);
 * - when that text is rebuilt (au_devwin_stale, au_devwin_refresh): the
 *   first four windows show the page the port names (the active tab of
 *   its front browser window) and follow it; the model never keeps a
 *   page pointer to use later, it only compares it;
 * - the window's parts and their rectangles for its size
 *   (au_devwin_areas): a tool band, the Network column titles, the
 *   text, the Network detail pane, the Console input line and the
 *   button bar. A port that lays the window out natively reads which
 *   parts a kind has from the same function, or from au_devwin_kind;
 * - the push buttons (au_devwin_buttons) and the check boxes or radio
 *   buttons of the tool band (au_devwin_toggles) with their labels,
 *   their state and what each does;
 * - the filter field and the Console's input line: their text, which
 *   of them has the keyboard, editing, history and running a line;
 * - the Network window's selected request and its detail text;
 * - Find in the window's text, and the text for Copy All and Save.
 *
 * Actions that need the system (the clipboard, saving a file, opening a
 * page, asking for the text to find, the machine's facts) call the port
 * through AuDevWinHost.
 *
 * To keep a window current, call au_devwin_refresh(dw, false) from the
 * event loop and redraw when it returns true. After any other call that
 * returns true (or AU_DEVWIN_DONE), redraw the window and set its scroll
 * bars from the text views. */
#ifndef AURA_BROWSER_DEVWIN_MODEL_H
#define AURA_BROWSER_DEVWIN_MODEL_H

#include "devtools.h"
#include "netlog.h"
#include "sourceview.h"
#include "sysinfo.h"
#include "textview.h"

typedef enum {
    AU_DEVWIN_SOURCE,
    AU_DEVWIN_CONSOLE,
    AU_DEVWIN_NETWORK,
    AU_DEVWIN_PERF,
    AU_DEVWIN_STATUS,
    AU_DEVWIN_REPORT,
    AU_DEVWIN_KINDS
} AuDevWinKind;

/* Heights in pixels of the parts that have a fixed one. */
enum {
    AU_DEVWIN_BAR_H = 32,    /* the button bar */
    AU_DEVWIN_TOOL_H = 24,   /* the tool band of Console and Network */
    AU_DEVWIN_REPORT_H = 40, /* Report a Problem's band: two lines of text */
    AU_DEVWIN_HEAD_H = 15,   /* the Network window's column titles */
    AU_DEVWIN_INPUT_H = 19,  /* the Console's input line */
    AU_DEVWIN_BUTTON_H = 20,
    AU_DEVWIN_TOGGLE_H = 16
};

/* Most bytes the filter field and the input line hold. */
#define AU_DEVWIN_FILTER_MAX 60
#define AU_DEVWIN_INPUT_MAX 4000
#define AU_DEVWIN_HISTORY 16 /* input lines Up and Down recall */
#define AU_DEVWIN_MAX_BUTTONS 3
#define AU_DEVWIN_MAX_TOGGLES 5

/* What a kind of window has. */
typedef struct AuDevWinKindInfo {
    const char *name;  /* its key in the develop.windows preference: "console" */
    const char *title; /* "Console"; the shown title adds the page's address */
    int default_h;     /* the height a new window opens at */
    bool filter;       /* a filter field in the tool band */
    bool band;         /* a tool band (filter, options or instructions) */
    bool head;         /* column titles over the text */
    bool detail;       /* a detail pane under the text */
    bool input;        /* an input line under the text */
    /* The text view's settings: wrap long lines, line numbers, a click
     * selects a row. */
    bool wrap, numbers, select_rows;
    /* Report a Problem: the two lines of instructions in its band (the
     * first is cut to the window's width); NULL for the others. */
    const char *band_text[2];
} AuDevWinKindInfo;

const AuDevWinKindInfo *au_devwin_kind(AuDevWinKind kind);

/* ---- Buttons and toggles ---- */

typedef enum {
    AU_DEVWIN_COPY_ALL,    /* the whole text to the clipboard (Copy Report in Report a Problem) */
    AU_DEVWIN_FIND,        /* asks for text (AuDevWinHost.ask_text) and finds it */
    AU_DEVWIN_CLEAR,       /* Console: clears the page's console */
    AU_DEVWIN_RUN_TIMING,  /* Performance: AuDevWinHost.run_timing */
    AU_DEVWIN_REFRESH,     /* System Status: rebuilds the report now */
    AU_DEVWIN_SAVE,        /* Report a Problem: the report to a file (AuDevWinHost.save_text) */
    AU_DEVWIN_OPEN_ISSUES, /* Report a Problem: AU_BUGREPORT_ISSUES_URL in a new tab */
    AU_DEVWIN_ACTIONS
} AuDevWinAction;

/* A push button of the bar, in the order the port shows them. width is
 * the Mac OS 9 window's button width; ports that size buttons to their
 * text ignore it. */
typedef struct AuDevWinButton {
    AuDevWinAction action;
    const char *label;
    int width;
} AuDevWinButton;

typedef enum {
    AU_DEVWIN_SHOW_ERRORS,      /* Console: list errors */
    AU_DEVWIN_SHOW_WARNINGS,    /* Console: list warnings */
    AU_DEVWIN_SHOW_LOGS,        /* Console: list log, info and debug lines */
    AU_DEVWIN_TRACE_SCROLLS,    /* Console: the page logs its scrolls (au_page_set_trace) */
    AU_DEVWIN_TRACE_NET,        /* Console: the page logs its requests and WebSockets (au_page_set_trace) */
    AU_DEVWIN_NET_ALL,          /* Network: every request (a radio group with the next two) */
    AU_DEVWIN_NET_PROBLEMS,     /* Network: requests that failed, were blocked or got 400 or more */
    AU_DEVWIN_NET_BLOCKED,      /* Network: blocked requests */
    AU_DEVWIN_REPORT_OTHER_TABS /* Report a Problem: list every open tab's address */
} AuDevWinToggleWhat;

/* A check box, or a radio button of the window's one radio group, in
 * the tool band. on is its state in a new window; width as for
 * buttons. */
typedef struct AuDevWinToggle {
    AuDevWinToggleWhat what;
    const char *label;
    int width;
    bool radio;
    bool on;
} AuDevWinToggle;

/* The kind's buttons and toggles. Return how many. */
int au_devwin_buttons(AuDevWinKind kind, const AuDevWinButton **out);
int au_devwin_toggles(AuDevWinKind kind, const AuDevWinToggle **out);

/* ---- What the port provides ---- */

struct AuBrowser;
struct AuBrowserWindow;

typedef struct AuDevWinHost {
    void *ctx; /* passed back */
    /* The page the window shows (the active tab of the front browser
     * window), or NULL. */
    AuPage *(*page)(void *ctx);
    /* The front browser window, for the bug report; NULL when none. */
    struct AuBrowserWindow *(*window)(void *ctx);
    /* Milliseconds from any fixed start. */
    uint32_t (*now_ms)(void *ctx);
    /* The machine's facts and what the shell and network stack know.
     * sys and h arrive filled with zeros; the model sets h->machine and
     * h->build. conn receives the connections text h->connection_text
     * may point into. May be NULL: every fact reads "unknown". */
    void (*host_info)(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn);
    /* Appends the open connections ("host:443 x2 (1 busy)"), for the
     * Network summary and Copy All. May be NULL. */
    void (*connections)(void *ctx, AuBuf *out);
    /* Appends the shell's own System Status sections (the network stack,
     * content blocking) in the report layout. May be NULL. */
    void (*status_sections)(void *ctx, AuBuf *out);
    /* The last timing test when it ran on page p, else NULL; the last
     * paint benchmark (ran false when none). Either may be NULL. */
    const AuTimingTest *(*timing)(void *ctx, const AuPage *p);
    const AuPaintBench *(*bench)(void *ctx);
    /* Puts text on the clipboard; false when it could not. */
    bool (*copy)(void *ctx, const char *text, size_t len);
    /* Asks where to save text and saves it: prompt, a suggested file
     * name. */
    void (*save_text)(void *ctx, const char *prompt, const char *name, const char *text, size_t len);
    /* Opens url in a new tab of the browser. */
    void (*open_url)(void *ctx, const char *url);
    /* Asks for a line of text: shows prompt with initial selected.
     * Returns the text (UTF-8, au_alloc'd, the caller frees it), or NULL
     * when the user cancelled. */
    char *(*ask_text)(void *ctx, const char *prompt, const char *initial);
    /* Runs the timing test on the front page and shows Performance. */
    void (*run_timing)(void *ctx);
    /* Shows (on) or ends (off) a busy cursor while a Console line runs.
     * May be NULL. */
    void (*busy)(void *ctx, bool on);
    /* The window's title changed (au_devwin_title). May be NULL. */
    void (*title_changed)(void *ctx, const char *title);
    /* Network: from now on the network stack keeps request and response
     * headers for the log. Called once, when the model is made. May be
     * NULL. */
    void (*record_headers)(void *ctx);
} AuDevWinHost;

/* ---- The model ---- */

typedef enum {
    AU_DEVWIN_PANE_NONE,
    AU_DEVWIN_PANE_FILTER, /* the filter field */
    AU_DEVWIN_PANE_HEAD,   /* the Network column titles */
    AU_DEVWIN_PANE_MAIN,   /* the text */
    AU_DEVWIN_PANE_DETAIL, /* the Network detail pane */
    AU_DEVWIN_PANE_INPUT   /* the Console input line */
} AuDevWinPane;

typedef struct AuDevRect {
    int left, top, right, bottom;
} AuDevRect;

typedef struct AuDevWinAreas {
    AuDevRect all, band, filter, head, main, detail, input, bar;
    bool has_band, has_filter, has_head, has_detail, has_input;
} AuDevWinAreas;

typedef enum {
    AU_DEVWIN_NOTHING, /* nothing to do, or cancelled */
    AU_DEVWIN_DONE,
    AU_DEVWIN_FAILED   /* the action could not be done: alert the user (the Mac beeps) */
} AuDevWinResult;

typedef struct AuDevWin {
    AuDevWinKind kind;
    struct AuBrowser *browser;
    AuFontSet *fonts;
    AuDevWinHost host;
    int width, height; /* the window's content, from au_devwin_resize */
    int scroll_w;      /* pixels each text pane leaves at its right for a scroll bar */
    /* The text: tv the main text, dv the Network detail pane. The port
     * paints them (au_textview_paint) at the main and detail areas'
     * top-left, sets their active and highlight fields, and reads their
     * scroll position for its scroll bars. */
    AuTextView tv, dv;
    char title[200];   /* "Console: https://example.com/" */
    char footer[320];  /* the status line after the buttons */
    char header[160];  /* Network: the column titles, painted at tv.gutter */
    /* The tool band's toggles, in au_devwin_toggles order. */
    bool toggle_on[AU_DEVWIN_MAX_TOGGLES];
    int ntoggles;
    /* The filter field (Console, Network) and the Console input line.
     * filter_focus: the filter field has the keyboard (in the Console the
     * input line has it otherwise; the Network window sends every typed
     * character to the filter). */
    AuBuf filter, input;
    bool filter_focus;
    char *find; /* the last text found; NULL before the first */
    /* Private. */
    AuTextList list, dlist;
    AuSourceView sv;
    bool have_sv;
    AuNetView nv;
    AuBuf text;          /* Performance, System Status, Report: the report shown */
    const void *page;    /* the page shown, compared and never used */
    const char *src_data;
    size_t src_len;
    uint32_t seen;       /* the console's line count or the log's generation */
    uint32_t last_ms;    /* when the text was last rebuilt */
    uint32_t detail_seq; /* Network: the request in the detail pane; 0 for none */
    bool built;          /* refreshed at least once */
    char *history[AU_DEVWIN_HISTORY];
    int nhistory, history_at;
} AuDevWin;

/* browser is the one System Status and the bug report describe; fonts
 * lay the text out. scroll_w: see AuDevWin. host may be NULL (no
 * page, no dialogs). Nothing is shown until au_devwin_resize and
 * au_devwin_refresh(dw, true). */
void au_devwin_init(AuDevWin *dw, AuDevWinKind kind, struct AuBrowser *browser, AuFontSet *fonts, int scroll_w,
                    const AuDevWinHost *host);
void au_devwin_free(AuDevWin *dw);

/* The window's content is now width by height pixels: lays the text out
 * again (the Network window builds its rows again, cut to the new
 * width). */
void au_devwin_resize(AuDevWin *dw, int width, int height);

/* The parts of the window for its current size. */
void au_devwin_areas(const AuDevWin *dw, AuDevWinAreas *a);
/* Where the port places button i and toggle i (Mac OS 9 layout: the
 * buttons from the left of the bar, the toggles after the filter field,
 * Report a Problem's at the right of its second line). */
void au_devwin_button_rect(const AuDevWin *dw, int i, AuDevRect *r);
void au_devwin_toggle_rect(const AuDevWin *dw, int i, AuDevRect *r);

/* True when what the window shows changed since the last refresh:
 *   Page Source  another page, or the page's source changed; a page
 *                still loading keeps the old text until its source is
 *                there
 *   Console      another page, or the page wrote a line
 *   Network      another page, or the log changed and 250 ms passed
 *   Performance  another page, or a second passed and no text is selected
 *   System Status  two seconds passed and no text is selected
 *   Report a Problem  never: it is a snapshot, built when the window is
 *                shown and when its toggle changes */
bool au_devwin_stale(const AuDevWin *dw);
/* Rebuilds the text when au_devwin_stale says so, or always with force.
 * Returns true when it did: redraw. */
bool au_devwin_refresh(AuDevWin *dw, bool force);

/* The title shown: the kind's title and the page's address. */
const char *au_devwin_title(const AuDevWin *dw);

/* Runs a button's action. Copy All with no text and Find with no match
 * return AU_DEVWIN_FAILED. */
AuDevWinResult au_devwin_run(AuDevWin *dw, AuDevWinAction action);
/* The user clicked toggle i: a check box turns over, a radio button is
 * chosen in its group. Refreshes the text. */
void au_devwin_toggle(AuDevWin *dw, int i);

/* ---- Keys and the mouse ----
 * Keys: key is an AU_KEY_* (AU_KEY_RETURN for Return and Enter), or
 * AU_KEY_NONE with text, the UTF-8 of a typed character. Returns
 * AU_DEVWIN_NOTHING when the window did not use the key; otherwise
 * redraw and set the scroll bars (AU_DEVWIN_FAILED: Return with no page
 * to run the line in; alert the user).
 *   - Console: Tab moves the keyboard between the filter field and the
 *     input line; Return runs the input line (or leaves the filter);
 *     Up and Down recall earlier input; the other arrows, Page Up, Page
 *     Down, Home and End scroll.
 *   - Network: typed text, Delete and Escape go to the filter field;
 *     the arrows move the selected request.
 *   - The others: the arrows and page keys scroll.
 *   - In a field, Delete removes the last character and Escape clears
 *     the field. */
AuDevWinResult au_devwin_key(AuDevWin *dw, int key, const char *text);
/* Edit > Paste into the field that has the keyboard: one line (line
 * breaks and tabs become spaces), dropped when it would overfill the
 * field. False when the window has no field. */
bool au_devwin_paste(AuDevWin *dw, const char *text);
/* Edit > Copy: appends the selection of the pane that has one. False
 * when nothing is selected. */
bool au_devwin_copy_text(AuDevWin *dw, AuBuf *out);
/* Edit > Select All in the main text; false for a list. */
bool au_devwin_select_all(AuDevWin *dw);

/* The part of the window at (x, y), window pixels. */
AuDevWinPane au_devwin_pane_at(const AuDevWin *dw, int x, int y);
/* A click at (x, y): the filter field takes the keyboard (a click
 * elsewhere gives it back to the Console's input line), a column title
 * sorts the requests, a click in the text selects (clicks: 1 to 3, as
 * au_textview_mouse_down; the port counts them) and in the Network list
 * shows the request's details. Returns the part clicked; redraw. When
 * it is a text pane whose view does not select rows, follow with
 * au_devwin_mouse_drag while the button is down and au_devwin_mouse_up. */
AuDevWinPane au_devwin_mouse_down(AuDevWin *dw, int x, int y, int clicks, bool shift);
/* Returns true when the selection or scroll changed: redraw. Past the
 * pane's top or bottom the text scrolls a row a call; pace the calls. */
bool au_devwin_mouse_drag(AuDevWin *dw, AuDevWinPane pane, int x, int y);
void au_devwin_mouse_up(AuDevWin *dw, AuDevWinPane pane);
/* The text view of a pane (MAIN or DETAIL), or NULL. */
AuTextView *au_devwin_view(AuDevWin *dw, AuDevWinPane pane);
/* The scroll bar of a pane: key (arrows, page keys) scrolls rows, even
 * in a list, whose arrow keys otherwise move its selection; scroll_to
 * shows row top first. */
void au_devwin_scroll(AuDevWin *dw, AuDevWinPane pane, int key);
void au_devwin_scroll_to(AuDevWin *dw, AuDevWinPane pane, uint32_t top);

/* ---- Find ---- */
/* Asks for the text (AuDevWinHost.ask_text) and finds it. NOTHING when
 * cancelled or empty, FAILED when not found. */
AuDevWinResult au_devwin_ask_find(AuDevWin *dw);
/* Remembers text and finds its next match. */
bool au_devwin_find(AuDevWin *dw, const char *text);
/* Finds the remembered text again after the selection, or before it;
 * wraps around. False when there is no text or no match. */
bool au_devwin_find_next(AuDevWin *dw, bool forward);
bool au_devwin_can_find_again(const AuDevWin *dw);

/* ---- The Network selection ---- */
/* The request on the selected row, or NULL. */
const AuNetRecord *au_devwin_network_selected(const AuDevWin *dw);
/* Selects row (-1 for none) and shows its details. */
void au_devwin_network_select(AuDevWin *dw, int32_t row);

/* ---- The Console input line ---- */
/* Runs the input line in the page: adds it to the history, clears it,
 * evaluates, refreshes and scrolls to the end. NOTHING for an empty
 * line; FAILED with no page. */
AuDevWinResult au_devwin_console_run(AuDevWin *dw);
/* Up (dir -1) and Down (1) through the history; past the newest the
 * line is empty. */
void au_devwin_console_recall(AuDevWin *dw, int dir);

/* ---- Copy All and Save ---- */
/* Appends the whole text: every request with its details and headers
 * (Network), every console line (Console), the report (Performance,
 * System Status, Report a Problem), the source (Page Source). */
void au_devwin_all_text(AuDevWin *dw, AuBuf *out);
/* Appends the report as Save Report writes it: CR LF line ends, which
 * editors on every system read. */
void au_devwin_save_text(const AuDevWin *dw, AuBuf *out);

#endif
