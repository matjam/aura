/* The bug report of Help > Report a Problem: everything a report needs,
 * as GitHub-flavoured Markdown to paste into a new issue. Portable C99.
 * Nothing here sends anything: the shell shows the text, and the person
 * copies or saves it.
 *
 * The text is ASCII only, so that it survives the Mac clipboard, a file
 * moved to another machine and a paste into a browser there: characters
 * outside ASCII are written as their nearest ASCII form or as \uXXXX.
 * Everything that comes from a page stands inside a fenced code block,
 * where GitHub notifies nobody and renders no markup.
 *
 * What it leaves out, always: cookies, request and response headers
 * (Authorization among them), request bodies and form data, the history,
 * bookmarks, the home page, the download folder and every other path on
 * the person's disk. In addresses, a user name and password are removed,
 * a query string or fragment longer than AU_BUGREPORT_QUERY_MAX
 * characters is cut there with "...", and a file: address keeps only its
 * file name. The addresses of tabs other than the active one are
 * included only when asked for. */
#ifndef AURA_BROWSER_BUGREPORT_H
#define AURA_BROWSER_BUGREPORT_H

#include "sysinfo.h"

#define AU_BUGREPORT_QUERY_MAX 100
#define AU_BUGREPORT_CONSOLE_LINES 50
#define AU_BUGREPORT_REQUESTS 200 /* table rows; with the limits in bugreport.c the report fits a GitHub issue */
#define AU_BUGREPORT_ISSUES_URL "https://github.com/matjam/aura/issues/new"

typedef struct AuBugReport {
    AuBrowser *browser;
    AuBrowserWindow *window; /* the frontmost browser window; NULL when none is open */
    const AuSysInfo *sys;    /* NULL: every machine fact reads "unknown" */
    const AuDevHostInfo *host; /* memory and the network's counters, for the Performance report; may be NULL */
    bool other_tabs;         /* list the address of every open tab, not only the active one */
} AuBugReport;

/* Appends the report: the template to fill in, Aura's version, the
 * machine, Aura's state, the active page, its console errors and
 * warnings, the request log (summary, and the table inside <details>),
 * the Performance report inside <details>, the preferences that differ
 * from the defaults, the clock, and a footer that says what was left
 * out. */
void au_bugreport_build(const AuBugReport *in, AuBuf *out);

/* Appends url as the report shows it (see the top of this file). */
void au_bugreport_redact_url(const char *url, AuBuf *out);
/* Appends text with every http, https, ftp and file address in it
 * redacted that way. */
void au_bugreport_redact_text(const char *text, AuBuf *out);
/* Appends text as ASCII: common punctuation as its ASCII form, other
 * characters as \uXXXX, control characters other than line feed and tab
 * as "?", and "```" as "'''" (so page text cannot close a code block). */
void au_bugreport_ascii(const char *text, size_t len, AuBuf *out);

#endif
