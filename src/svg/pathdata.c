/* SVG attribute grammars: numbers, path data, transform lists, lengths,
 * number lists and preserveAspectRatio. All parsers walk NUL-terminated
 * text once, never recurse, and clamp numbers to a finite range. */
#include "svg_int.h"

#define NUM_LIMIT 1e9

void svg_skip_sep(const char **p)
{
    const char *s = *p;
    while (svg_ws(*s))
        s++;
    if (*s == ',') {
        s++;
        while (svg_ws(*s))
            s++;
    }
    *p = s;
}

bool svg_parse_number(const char **p, float *out)
{
    const char *s = *p;
    bool neg = false;
    if (*s == '+' || *s == '-')
        neg = *s++ == '-';
    double v = 0;
    bool digits = false;
    while (*s >= '0' && *s <= '9') {
        if (v < 1e300)
            v = v * 10 + (*s - '0');
        s++;
        digits = true;
    }
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') {
        s++;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s - '0') * f;
            f *= 0.1;
            s++;
        }
        digits = true;
    } else if (*s == '.' && digits) {
        s++; /* "5." */
    }
    if (!digits)
        return false;
    /* An exponent only when digits follow (so "1em" is 1 with a unit). */
    if ((*s == 'e' || *s == 'E') &&
        ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '+' || s[1] == '-') && s[2] >= '0' && s[2] <= '9'))) {
        s++;
        bool eneg = false;
        if (*s == '+' || *s == '-')
            eneg = *s++ == '-';
        int e = 0;
        while (*s >= '0' && *s <= '9') {
            if (e < 1000)
                e = e * 10 + (*s - '0');
            s++;
        }
        if (e > 300)
            e = 300;
        v = eneg ? v / pow(10, e) : v * pow(10, e);
    }
    if (!(v < NUM_LIMIT))
        v = NUM_LIMIT;
    *out = (float)(neg ? -v : v);
    *p = s;
    return true;
}

/* A flag of an arc: a single 0 or 1, which may run into what follows. */
static bool parse_flag(const char **p, bool *out)
{
    const char *s = *p;
    if (*s != '0' && *s != '1')
        return false;
    *out = *s == '1';
    *p = s + 1;
    return true;
}

static int arg_count(char cmd)
{
    switch (cmd | 0x20) {
    case 'm': case 'l': case 't': return 2;
    case 'h': case 'v': return 1;
    case 'c': return 6;
    case 's': case 'q': return 4;
    case 'a': return 7;
    case 'z': return 0;
    default: return -1;
    }
}

bool au_svg_path_parse(const char *d, size_t len, AuPath *out)
{
    (void)len;
    const char *s = d;
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0; /* current point, subpath start */
    float lcx = 0, lcy = 0;               /* last control point */
    char prev = 0;                        /* previous command (lowercase) */
    bool started = false;
    for (;;) {
        while (svg_ws(*s))
            s++;
        if (!*s)
            return true;
        char c = *s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            if (arg_count(c) < 0)
                return false;
            cmd = c;
            s++;
            if (!started && (cmd | 0x20) != 'm')
                return false; /* the first command must be a moveto */
        } else {
            /* An implicit repeat: needs a command that takes arguments. */
            if (!cmd || arg_count(cmd) == 0)
                return false;
        }
        int n = arg_count(cmd);
        float a[7] = {0};
        bool flags[2] = {false, false};
        for (int i = 0; i < n; i++) {
            while (svg_ws(*s))
                s++;
            bool ok;
            if ((cmd | 0x20) == 'a' && (i == 3 || i == 4))
                ok = parse_flag(&s, &flags[i - 3]);
            else
                ok = svg_parse_number(&s, &a[i]);
            if (!ok)
                return false;
            if (i + 1 < n)
                svg_skip_sep(&s);
        }
        bool rel = cmd >= 'a';
        char lc = (char)(cmd | 0x20);
        switch (lc) {
        case 'm': {
            /* A relative moveto at the start is relative to (0, 0). */
            float x = rel ? cx + a[0] : a[0], y = rel ? cy + a[1] : a[1];
            au_path_move_to(out, x, y);
            cx = sx = x;
            cy = sy = y;
            started = true;
            /* Further pairs are linetos. */
            cmd = rel ? 'l' : 'L';
            break;
        }
        case 'l': {
            float x = rel ? cx + a[0] : a[0], y = rel ? cy + a[1] : a[1];
            au_path_line_to(out, x, y);
            cx = x, cy = y;
            break;
        }
        case 'h': {
            float x = rel ? cx + a[0] : a[0];
            au_path_line_to(out, x, cy);
            cx = x;
            break;
        }
        case 'v': {
            float y = rel ? cy + a[0] : a[0];
            au_path_line_to(out, cx, y);
            cy = y;
            break;
        }
        case 'c': {
            float ox = rel ? cx : 0, oy = rel ? cy : 0;
            au_path_cubic_to(out, ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]);
            lcx = ox + a[2], lcy = oy + a[3];
            cx = ox + a[4], cy = oy + a[5];
            break;
        }
        case 's': {
            float ox = rel ? cx : 0, oy = rel ? cy : 0;
            float c1x = cx, c1y = cy;
            if (prev == 'c' || prev == 's') {
                c1x = 2 * cx - lcx;
                c1y = 2 * cy - lcy;
            }
            au_path_cubic_to(out, c1x, c1y, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
            lcx = ox + a[0], lcy = oy + a[1];
            cx = ox + a[2], cy = oy + a[3];
            break;
        }
        case 'q': {
            float ox = rel ? cx : 0, oy = rel ? cy : 0;
            au_path_quad_to(out, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
            lcx = ox + a[0], lcy = oy + a[1];
            cx = ox + a[2], cy = oy + a[3];
            break;
        }
        case 't': {
            float ox = rel ? cx : 0, oy = rel ? cy : 0;
            float qx = cx, qy = cy;
            if (prev == 'q' || prev == 't') {
                qx = 2 * cx - lcx;
                qy = 2 * cy - lcy;
            }
            au_path_quad_to(out, qx, qy, ox + a[0], oy + a[1]);
            lcx = qx, lcy = qy;
            cx = ox + a[0], cy = oy + a[1];
            break;
        }
        case 'a': {
            float x = rel ? cx + a[5] : a[5], y = rel ? cy + a[6] : a[6];
            out->cx = cx; /* the arc starts at the current point */
            out->cy = cy;
            au_path_arc_svg(out, a[0], a[1], a[2], flags[0], flags[1], x, y);
            cx = x, cy = y;
            break;
        }
        case 'z':
            au_path_close(out);
            cx = sx, cy = sy;
            break;
        }
        prev = lc == 'm' ? 'l' : lc;
        if (lc == 'z')
            prev = 'z';
        svg_skip_sep(&s);
    }
}

/* ---- Transform lists ---- */

static const char *match_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return strncmp(s, w, n) == 0 ? s + n : NULL;
}

bool au_svg_transform_parse(const char *s, AuMatrix *out)
{
    AuMatrix m = {1, 0, 0, 1, 0, 0};
    *out = m;
    if (!s)
        return false;
    int guard = 0;
    for (;;) {
        while (svg_ws(*s) || *s == ',')
            s++;
        if (!*s)
            break;
        if (++guard > 256)
            return false;
        static const struct {
            const char *name;
            int min, max;
        } fns[] = {{"matrix", 6, 6}, {"translate", 1, 2}, {"scale", 1, 2}, {"rotate", 1, 3}, {"skewX", 1, 1},
                   {"skewY", 1, 1}};
        int which = -1;
        const char *q = NULL;
        for (int i = 0; i < 6 && which < 0; i++)
            if ((q = match_word(s, fns[i].name)) != NULL)
                which = i;
        if (which < 0)
            return false;
        s = q;
        while (svg_ws(*s))
            s++;
        if (*s != '(')
            return false;
        s++;
        float a[6] = {0};
        int n = 0;
        while (svg_ws(*s))
            s++;
        while (*s != ')') {
            if (n == 6 || !svg_parse_number(&s, &a[n]))
                return false;
            n++;
            svg_skip_sep(&s);
        }
        s++;
        if (n < fns[which].min || n > fns[which].max || (which == 3 && n == 2))
            return false;
        AuMatrix t = {1, 0, 0, 1, 0, 0};
        float rad;
        switch (which) {
        case 0: t = (AuMatrix){a[0], a[1], a[2], a[3], a[4], a[5]}; break;
        case 1: t.e = a[0]; t.f = n > 1 ? a[1] : 0; break;
        case 2: t.a = a[0]; t.d = n > 1 ? a[1] : a[0]; break;
        case 3:
            rad = a[0] * 3.14159265358979f / 180.0f;
            t = (AuMatrix){cosf(rad), sinf(rad), -sinf(rad), cosf(rad), 0, 0};
            if (n == 3) {
                AuMatrix pre = {1, 0, 0, 1, a[1], a[2]}, post = {1, 0, 0, 1, -a[1], -a[2]};
                au_matrix_multiply(&pre, &t);
                au_matrix_multiply(&pre, &post);
                t = pre;
            }
            break;
        case 4: t.c = tanf(a[0] * 3.14159265358979f / 180.0f); break;
        case 5: t.b = tanf(a[0] * 3.14159265358979f / 180.0f); break;
        }
        au_matrix_multiply(&m, &t);
    }
    if (!(isfinite(m.a) && isfinite(m.b) && isfinite(m.c) && isfinite(m.d) && isfinite(m.e) && isfinite(m.f)))
        return false;
    *out = m;
    return true;
}

/* ---- Lengths ---- */

bool svg_parse_length(const char *s, SvgLen *out)
{
    if (!s)
        return false;
    while (svg_ws(*s))
        s++;
    float v;
    if (!svg_parse_number(&s, &v))
        return false;
    static const struct {
        const char *u;
        uint8_t id;
    } units[] = {{"px", SVG_U_PX}, {"pt", SVG_U_PT}, {"pc", SVG_U_PC}, {"mm", SVG_U_MM}, {"cm", SVG_U_CM},
                 {"in", SVG_U_IN}, {"em", SVG_U_EM}, {"ex", SVG_U_EX}, {"%", SVG_U_PCT}};
    uint8_t unit = SVG_U_NUM;
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        size_t n = strlen(units[i].u);
        if (strncmp(s, units[i].u, n) == 0) {
            unit = units[i].id;
            s += n;
            break;
        }
    }
    while (svg_ws(*s))
        s++;
    if (*s)
        return false;
    out->v = v;
    out->unit = unit;
    return true;
}

float svg_len_px(SvgLen l, float em, float ref)
{
    switch (l.unit) {
    case SVG_U_PT: return l.v * 96.0f / 72.0f;
    case SVG_U_PC: return l.v * 16.0f;
    case SVG_U_MM: return l.v * 96.0f / 25.4f;
    case SVG_U_CM: return l.v * 96.0f / 2.54f;
    case SVG_U_IN: return l.v * 96.0f;
    case SVG_U_EM: return l.v * em;
    case SVG_U_EX: return l.v * em * 0.5f;
    case SVG_U_PCT: return l.v * ref / 100.0f;
    default: return l.v;
    }
}

size_t svg_parse_numbers(const char *s, float *out, size_t cap, bool *ok)
{
    size_t n = 0;
    *ok = true;
    if (!s)
        return 0;
    while (svg_ws(*s))
        s++;
    while (*s) {
        float v;
        if (!svg_parse_number(&s, &v)) {
            *ok = false;
            break;
        }
        if (n < cap)
            out[n] = v;
        n++;
        svg_skip_sep(&s);
    }
    return n < cap ? n : cap;
}

size_t svg_parse_lengths(const char *s, SvgLen *out, size_t cap)
{
    size_t n = 0;
    if (!s)
        return 0;
    char buf[64];
    while (*s && n < cap) {
        while (svg_ws(*s) || *s == ',')
            s++;
        const char *b = s;
        while (*s && !svg_ws(*s) && *s != ',')
            s++;
        size_t len = (size_t)(s - b);
        if (!len)
            break;
        if (len >= sizeof buf)
            return n;
        memcpy(buf, b, len);
        buf[len] = 0;
        if (!svg_parse_length(buf, &out[n]))
            return n;
        n++;
    }
    return n;
}

/* ---- preserveAspectRatio ---- */

void svg_parse_par(const char *s, SvgPar *out)
{
    out->ax = out->ay = SVG_ALIGN_MID;
    out->slice = false;
    if (!s)
        return;
    while (svg_ws(*s))
        s++;
    if (strncmp(s, "defer", 5) == 0) {
        s += 5;
        while (svg_ws(*s))
            s++;
    }
    if (strncmp(s, "none", 4) == 0) {
        out->ax = out->ay = SVG_ALIGN_NONE;
        return;
    }
    if (s[0] == 'x' && s[4] == 'Y' && strlen(s) >= 8) {
        uint8_t ax = 0, ay = 0;
        if (strncmp(s + 1, "Min", 3) == 0) ax = SVG_ALIGN_MIN;
        else if (strncmp(s + 1, "Mid", 3) == 0) ax = SVG_ALIGN_MID;
        else if (strncmp(s + 1, "Max", 3) == 0) ax = SVG_ALIGN_MAX;
        if (strncmp(s + 5, "Min", 3) == 0) ay = SVG_ALIGN_MIN;
        else if (strncmp(s + 5, "Mid", 3) == 0) ay = SVG_ALIGN_MID;
        else if (strncmp(s + 5, "Max", 3) == 0) ay = SVG_ALIGN_MAX;
        if (!ax || !ay)
            return;
        out->ax = ax;
        out->ay = ay;
        s += 8;
        while (svg_ws(*s))
            s++;
        if (strncmp(s, "slice", 5) == 0)
            out->slice = true;
    }
}

AuMatrix svg_viewbox_matrix(const float vb[4], AuRect vp, SvgPar par)
{
    float sx = vp.w / vb[2], sy = vp.h / vb[3];
    if (par.ax == SVG_ALIGN_NONE)
        return (AuMatrix){sx, 0, 0, sy, vp.x - vb[0] * sx, vp.y - vb[1] * sy};
    float s = par.slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    float tx = vp.x - vb[0] * s, ty = vp.y - vb[1] * s;
    float ex = vp.w - vb[2] * s, ey = vp.h - vb[3] * s;
    if (par.ax == SVG_ALIGN_MID)
        tx += ex / 2;
    else if (par.ax == SVG_ALIGN_MAX)
        tx += ex;
    if (par.ay == SVG_ALIGN_MID)
        ty += ey / 2;
    else if (par.ay == SVG_ALIGN_MAX)
        ty += ey;
    return (AuMatrix){s, 0, 0, s, tx, ty};
}
