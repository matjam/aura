/* The control socket's commands: a test driver (tools/aura-ctl.py) or a
 * developer runs a browser window from outside, one line at a time. The
 * port listens and passes each line here (the Cocoa port with
 * AURA_CONTROL set; docs/developer-tools.md, "The control socket").
 * Portable C99.
 *
 * Coordinates are in the window's page area, the page's viewport (0,0 is
 * its top left whatever chrome is around it). The reply starts "ok" or
 * "error", then the command's text:
 *
 *   eval EXPR                  run EXPR in the page ("@N EXPR", "@text EXPR"
 *                              in a frame, "@" lists the frames)
 *                              "error busy" while the page's script is
 *                              parked in a long task; "eval! EXPR" runs
 *                              that task to its end first (stopped after
 *                              10 s more)
 *   where [@FRAME] SELECTOR    the first element matching SELECTOR, as
 *                              "X Y W H" (scrolled into view first)
 *   clicksel [@FRAME] SELECTOR click the middle of that element
 *   click X Y | dclick X Y     press and release (twice for dclick)
 *   down X Y | up X Y | move X Y
 *   drag X1 Y1 X2 Y2 [STEPS]   press, move in STEPS (10) steps letting the
 *                              page run after each, release
 *   key [cmd-][shift-][opt-][ctrl-]NAME   a named key or one character
 *   type TEXT                  each character as typed
 *   go URL | reload | back | forward
 *   url | title | busy         the active tab's address, title, whether it loads
 *   console [N]                the last N (30) console lines, "LEVEL text"
 *   trace [net] [scroll]       the Console's Trace Network and Trace Scrolls
 *                              ("trace off": neither)
 *   net [details]              the page's request log, as Copy All in the
 *                              Network window gives it
 *   idle [N]                   lets the window run N (10) idle turns
 *
 * The port handles "shot PATH" itself (a picture of the window), and
 * "headers" (record request and response headers from now on, as the
 * Network window does, for "net details"). */
#ifndef AURA_BROWSER_CONTROL_H
#define AURA_BROWSER_CONTROL_H

#include "browser.h"

/* Runs one command line on w; out gets the reply. False for an error
 * (out then starts "error"). */
bool au_control_run(AuBrowserWindow *w, const char *line, AuBuf *out);

#endif
