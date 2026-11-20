/* The control socket's commands (control.h). Portable C99. */
#include "control.h"
#include "netlog.h"
#include "page.h"

#include <stdio.h>
#include <string.h>

static AuPage *active_page(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    return t ? t->page : NULL;
}

static void mouse(AuBrowserWindow *w, AuEventType type, int x, int y, int clicks)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.x = w->layout.page.x0 + x;
    e.y = w->layout.page.y0 + y;
    e.clicks = clicks;
    au_bw_mouse(w, &e);
}

static void idle(AuBrowserWindow *w, int n)
{
    for (int i = 0; i < n; i++)
        au_bw_idle(w);
}

static void click(AuBrowserWindow *w, int x, int y, int clicks)
{
    mouse(w, AU_EVENT_MOUSE_MOVE, x, y, 0);
    for (int k = 1; k <= clicks; k++) {
        mouse(w, AU_EVENT_MOUSE_DOWN, x, y, k);
        mouse(w, AU_EVENT_MOUSE_UP, x, y, k);
    }
}

static void key(AuBrowserWindow *w, const char *s)
{
    uint32_t mods = 0;
    for (;;) {
        if (!strncmp(s, "cmd-", 4))
            mods |= AU_MOD_COMMAND, s += 4;
        else if (!strncmp(s, "shift-", 6))
            mods |= AU_MOD_SHIFT, s += 6;
        else if (!strncmp(s, "opt-", 4))
            mods |= AU_MOD_OPTION, s += 4;
        else if (!strncmp(s, "ctrl-", 5))
            mods |= AU_MOD_CONTROL, s += 5;
        else
            break;
    }
    static const struct {
        const char *name;
        int key;
    } keys[] = {{"left", AU_KEY_LEFT},         {"right", AU_KEY_RIGHT},   {"up", AU_KEY_UP},
                {"down", AU_KEY_DOWN},         {"pageup", AU_KEY_PAGE_UP}, {"pagedown", AU_KEY_PAGE_DOWN},
                {"home", AU_KEY_HOME},         {"end", AU_KEY_END},       {"backspace", AU_KEY_BACKSPACE},
                {"delete", AU_KEY_DELETE},     {"return", AU_KEY_RETURN}, {"tab", AU_KEY_TAB},
                {"escape", AU_KEY_ESCAPE}};
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.mods = mods;
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
        if (!strcmp(s, keys[k].name))
            e.key = keys[k].key;
    if (!e.key) {
        size_t adv = 0;
        e.ch = !strcmp(s, "space") ? ' ' : au_utf8_decode(s, strlen(s), &adv);
    }
    au_bw_key(w, &e);
}

static void type(AuBrowserWindow *w, const char *s)
{
    size_t n = strlen(s), i = 0;
    while (i < n) {
        size_t adv = 1;
        uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
        i += adv ? adv : 1;
        AuEvent e;
        memset(&e, 0, sizeof e);
        e.type = AU_EVENT_KEY_DOWN;
        if (cp == '\n')
            e.key = AU_KEY_RETURN;
        else
            e.ch = cp;
        au_bw_key(w, &e);
    }
}

/* "[@FRAME] SELECTOR": the frame's name (empty for the page) and the
 * selector. */
static const char *split_frame(const char *arg, char *frame, size_t cap)
{
    frame[0] = 0;
    if (arg[0] != '@')
        return arg;
    const char *sp = strchr(arg, ' ');
    if (!sp)
        return "";
    size_t n = (size_t)(sp - arg - 1);
    snprintf(frame, cap, "%.*s", (int)(n < cap ? n : cap - 1), arg + 1);
    return sp + 1;
}

static bool error(AuBuf *out, const char *msg)
{
    au_buf_puts(out, "error ");
    au_buf_puts(out, msg);
    return false;
}

bool au_control_run(AuBrowserWindow *w, const char *line, AuBuf *out)
{
    char cmd[16];
    size_t n = strcspn(line, " ");
    if (n >= sizeof cmd)
        return error(out, "unknown command");
    memcpy(cmd, line, n);
    cmd[n] = 0;
    const char *arg = line[n] ? line + n + 1 : "";
    AuPage *p = active_page(w);
    int x = 0, y = 0, x2 = 0, y2 = 0, steps = 10;

    if (!strcmp(cmd, "eval")) {
        if (!p)
            return error(out, "no tab");
        AuBuf r;
        au_buf_init(&r);
        bool ok = au_page_eval(p, arg, &r);
        au_buf_puts(out, ok ? "ok " : "error ");
        au_buf_append(out, r.data, r.len);
        au_buf_free(&r);
        return ok;
    }
    if (!strcmp(cmd, "where") || !strcmp(cmd, "clicksel")) {
        char frame[200];
        const char *sel = split_frame(arg, frame, sizeof frame);
        AuIRect r;
        if (!p || !*sel || !au_page_element_rect(p, frame, sel, &r))
            return error(out, "no such element");
        if (cmd[0] == 'c')
            click(w, (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2, 1);
        au_buf_printf(out, "ok %d %d %d %d", r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
        return true;
    }
    if (!strcmp(cmd, "click") || !strcmp(cmd, "dclick") || !strcmp(cmd, "down") || !strcmp(cmd, "up") ||
        !strcmp(cmd, "move")) {
        if (sscanf(arg, "%d %d", &x, &y) != 2)
            return error(out, "needs X Y");
        if (!strcmp(cmd, "click"))
            click(w, x, y, 1);
        else if (!strcmp(cmd, "dclick"))
            click(w, x, y, 2);
        else
            mouse(w, cmd[0] == 'd' ? AU_EVENT_MOUSE_DOWN : cmd[0] == 'u' ? AU_EVENT_MOUSE_UP : AU_EVENT_MOUSE_MOVE, x,
                  y, cmd[0] == 'm' ? 0 : 1);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "drag")) {
        if (sscanf(arg, "%d %d %d %d %d", &x, &y, &x2, &y2, &steps) < 4)
            return error(out, "needs X1 Y1 X2 Y2 [STEPS]");
        if (steps < 1 || steps > 200)
            steps = 10;
        mouse(w, AU_EVENT_MOUSE_MOVE, x, y, 0);
        mouse(w, AU_EVENT_MOUSE_DOWN, x, y, 1);
        idle(w, 2);
        for (int k = 1; k <= steps; k++) {
            mouse(w, AU_EVENT_MOUSE_MOVE, x + (x2 - x) * k / steps, y + (y2 - y) * k / steps, 0);
            idle(w, 2);
        }
        mouse(w, AU_EVENT_MOUSE_UP, x2, y2, 1);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "key")) {
        if (!*arg)
            return error(out, "needs a key");
        key(w, arg);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "type")) {
        type(w, arg);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "go")) {
        if (!*arg)
            return error(out, "needs a URL");
        au_bw_navigate_input(w, arg);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "reload") || !strcmp(cmd, "back") || !strcmp(cmd, "forward")) {
        if (cmd[0] == 'r')
            au_bw_reload(w);
        else if (cmd[0] == 'b')
            au_bw_back(w);
        else
            au_bw_forward(w);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "url") || !strcmp(cmd, "title")) {
        if (!p)
            return error(out, "no tab");
        au_buf_printf(out, "ok %s", cmd[0] == 'u' ? au_page_url(p) : au_page_title(p));
        return true;
    }
    if (!strcmp(cmd, "busy")) {
        au_buf_printf(out, "ok %s", au_browser_busy(w->browser) ? "yes" : "no");
        return true;
    }
    if (!strcmp(cmd, "idle")) {
        int k = 10;
        sscanf(arg, "%d", &k);
        idle(w, k < 1 ? 1 : k > 10000 ? 10000 : k);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "trace")) {
        /* "trace net scroll", "trace off": the Console's Trace boxes */
        unsigned f = 0;
        if (strstr(arg, "net"))
            f |= AU_PAGE_TRACE_NET;
        if (strstr(arg, "scroll"))
            f |= AU_PAGE_TRACE_SCROLL;
        if (!p)
            return error(out, "no tab");
        au_page_set_trace(p, f);
        au_buf_puts(out, "ok");
        return true;
    }
    if (!strcmp(cmd, "net")) {
        if (!p || !au_page_netlog(p))
            return error(out, "no tab");
        au_buf_puts(out, "ok\n");
        au_netlog_dump(au_page_netlog(p), "", strstr(arg, "details") != NULL, out);
        return true;
    }
    if (!strcmp(cmd, "console")) {
        if (!p)
            return error(out, "no tab");
        int want = 30;
        sscanf(arg, "%d", &want);
        size_t count = au_page_console_count(p);
        size_t from = want > 0 && (size_t)want < count ? count - (size_t)want : 0;
        static const char *const names[] = {"log", "info", "warn", "error", "debug"};
        au_buf_puts(out, "ok");
        for (size_t i = from; i < count; i++) {
            int level = 0;
            const char *t = au_page_console_line(p, i, &level);
            au_buf_printf(out, "\n%s %s", names[level >= 0 && level < 5 ? level : 0], t ? t : "");
        }
        return true;
    }
    return error(out, "unknown command");
}
