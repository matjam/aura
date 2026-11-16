/* Variable TrueType fonts: fvar (axes, named instances), avar (segment
 * maps), gvar (glyph outline deltas with interpolation of untouched
 * points), HVAR (advance deltas), MVAR (metric deltas) and the parts of
 * STAT that name an instance.
 *
 * The tables come from the network: every read is bounds-checked through
 * the au_fd_* readers, counts are capped, offset arithmetic saturates, and
 * the work one glyph can cause is bounded. A malformed table disables
 * variation (the default instance draws) or fails one glyph; it never
 * reads outside the font. */
#include "font_internal.h"

#include <math.h>
#include <string.h>

struct AuFontVar {
    uint32_t refs;
    int naxes;
    AuFontAxis axes[AU_FONT_MAX_AXES];
    uint32_t inst;           /* first named instance record */
    uint16_t ninst, inst_size;
    /* avar: the segment map of each axis (offset, count); count 0: none */
    uint32_t avar_map[AU_FONT_MAX_AXES];
    uint16_t avar_n[AU_FONT_MAX_AXES];
    /* gvar */
    bool gvar_ok, gvar_long;
    uint16_t gvar_glyphs, nshared;
    uint32_t shared_tuples, glyph_data, gvar_offsets;
    /* HVAR: the item variation store and the advance mapping (0: none) */
    uint32_t hvar_store, hvar_map;
    /* MVAR */
    uint32_t mvar_store, mvar_recs;
    uint16_t mvar_nrec, mvar_recsize;
    int16_t def_metrics[9]; /* the default instance's metrics, for instances */
};

/* Limits that no real font reaches. */
#define MAX_REGIONS_PER_ROW 512   /* regions an item variation row may name */
#define MAX_DELTA_WORK 4000000    /* deltas decoded for one glyph */
#define MAX_GVAR_POINTS 20004     /* outline points (glyf's limit) plus phantoms */

#define F2DOT14 16384.0f

static uint32_t mul_off(uint32_t base, uint32_t a, uint32_t b)
{
    uint64_t v = (uint64_t)base + (uint64_t)a * b;
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

void au_fontvar_retain(AuFontVar *v)
{
    if (v)
        v->refs++;
}

void au_fontvar_release(AuFontVar *v)
{
    if (v && --v->refs == 0)
        au_free(v);
}

/* ---- Setup ---- */

static void parse_fvar(const AuFontFace *f, AuFontVar *v)
{
    uint32_t b = f->fvar;
    if (f->fvar_len < 16 || au_fd_u16(f, b) != 1)
        return;
    uint16_t axes_off = au_fd_u16(f, b + 4), naxes = au_fd_u16(f, b + 8), axis_size = au_fd_u16(f, b + 10);
    uint16_t ninst = au_fd_u16(f, b + 12), inst_size = au_fd_u16(f, b + 14);
    if (!naxes || naxes > AU_FONT_MAX_AXES || axis_size < 20)
        return;
    uint32_t end = b + f->fvar_len;
    uint32_t a = b + axes_off;
    if (mul_off(a, naxes, axis_size) > end)
        return;
    for (int i = 0; i < naxes; i++) {
        uint32_t r = a + (uint32_t)i * axis_size;
        AuFontAxis *ax = &v->axes[i];
        ax->tag = au_fd_u32(f, r);
        ax->min = au_fd_s32(f, r + 4) / 65536.0f;
        ax->def = au_fd_s32(f, r + 8) / 65536.0f;
        ax->max = au_fd_s32(f, r + 12) / 65536.0f;
        ax->hidden = (au_fd_u16(f, r + 16) & 1) != 0;
        ax->name_id = au_fd_u16(f, r + 18);
        if (!(ax->min <= ax->def && ax->def <= ax->max))
            return; /* malformed: no variation */
    }
    v->naxes = naxes;
    uint32_t inst = a + (uint32_t)naxes * axis_size;
    if (inst_size >= 4 + (uint32_t)naxes * 4 && mul_off(inst, ninst, inst_size) <= end) {
        v->inst = inst;
        v->ninst = ninst;
        v->inst_size = inst_size;
    }
}

static void parse_avar(const AuFontFace *f, AuFontVar *v)
{
    uint32_t b = f->avar;
    /* Version 1, or 2 (whose first part is the same segment maps). */
    if (f->avar_len < 8 || (au_fd_u16(f, b) != 1 && au_fd_u16(f, b) != 2))
        return;
    if (au_fd_u16(f, b + 6) != v->naxes)
        return;
    uint32_t end = b + f->avar_len, p = b + 8;
    for (int i = 0; i < v->naxes; i++) {
        if (p + 2 > end)
            return;
        uint16_t n = au_fd_u16(f, p);
        if (mul_off(p + 2, n, 4) > end)
            return;
        v->avar_map[i] = p + 2;
        v->avar_n[i] = n;
        p += 2 + (uint32_t)n * 4;
    }
}

static void parse_gvar(const AuFontFace *f, AuFontVar *v)
{
    uint32_t b = f->gvar;
    if (f->gvar_len < 20 || au_fd_u16(f, b) != 1)
        return;
    if (au_fd_u16(f, b + 4) != v->naxes)
        return;
    uint16_t nshared = au_fd_u16(f, b + 6);
    uint32_t shared = au_fd_add(b, au_fd_u32(f, b + 8));
    uint16_t nglyphs = au_fd_u16(f, b + 12), flags = au_fd_u16(f, b + 14);
    uint32_t data = au_fd_add(b, au_fd_u32(f, b + 16));
    uint32_t end = b + f->gvar_len;
    bool lng = flags & 1;
    if (mul_off(b + 20, (uint32_t)nglyphs + 1, lng ? 4 : 2) > end)
        return;
    if (mul_off(shared, nshared, (uint32_t)v->naxes * 2) > end)
        return;
    if (data > end)
        return;
    v->gvar_ok = true;
    v->gvar_long = lng;
    v->gvar_glyphs = nglyphs;
    v->nshared = nshared;
    v->shared_tuples = shared;
    v->glyph_data = data;
    v->gvar_offsets = b + 20;
}

/* An item variation store at off within a table [tb, tb + tlen); 0 when it
 * is not usable. */
static uint32_t check_store(const AuFontFace *f, uint32_t tb, uint32_t tlen, uint32_t off)
{
    if (!off || off >= tlen)
        return 0;
    uint32_t s = tb + off;
    if (au_fd_u16(f, s) != 1 || !au_fd_in(f, s, 8))
        return 0;
    return s;
}

static void parse_hvar(const AuFontFace *f, AuFontVar *v)
{
    uint32_t b = f->hvar;
    if (f->hvar_len < 20 || au_fd_u16(f, b) != 1)
        return;
    v->hvar_store = check_store(f, b, f->hvar_len, au_fd_u32(f, b + 4));
    uint32_t map = au_fd_u32(f, b + 8);
    if (v->hvar_store && map && map < f->hvar_len)
        v->hvar_map = b + map;
}

static void parse_mvar(const AuFontFace *f, AuFontVar *v)
{
    uint32_t b = f->mvar;
    if (f->mvar_len < 12 || au_fd_u16(f, b) != 1)
        return;
    uint16_t size = au_fd_u16(f, b + 6), n = au_fd_u16(f, b + 8);
    uint32_t store = check_store(f, b, f->mvar_len, au_fd_u16(f, b + 10));
    if (size < 8 || !store || mul_off(b + 12, n, size) > b + f->mvar_len)
        return;
    v->mvar_store = store;
    v->mvar_recs = b + 12;
    v->mvar_nrec = n;
    v->mvar_recsize = size;
}

void au_fontvar_init(AuFontFace *f)
{
    if (!f->fvar_len)
        return;
    AuFontVar *v = au_calloc(sizeof *v);
    v->refs = 1;
    parse_fvar(f, v);
    if (!v->naxes) {
        au_free(v);
        return;
    }
    parse_avar(f, v);
    parse_gvar(f, v);
    parse_hvar(f, v);
    parse_mvar(f, v);
    int16_t *dm = v->def_metrics;
    dm[0] = f->ascender, dm[1] = f->descender, dm[2] = f->line_gap, dm[3] = f->x_height, dm[4] = f->cap_height;
    dm[5] = f->underline_pos, dm[6] = f->underline_thickness, dm[7] = f->strike_pos, dm[8] = f->strike_thickness;
    f->var = v;
}

/* ---- Axes and normalization ---- */

int au_font_axis_count(const AuFontFace *f)
{
    return f->var ? f->var->naxes : 0;
}

const AuFontAxis *au_font_axis(const AuFontFace *f, int i)
{
    return f->var && i >= 0 && i < f->var->naxes ? &f->var->axes[i] : NULL;
}

int au_font_axis_find(const AuFontFace *f, uint32_t tag)
{
    for (int i = 0; f->var && i < f->var->naxes; i++)
        if (f->var->axes[i].tag == tag)
            return i;
    return -1;
}

int au_font_normalize(const AuFontFace *f, int axis, float value)
{
    if (!f->var || axis < 0 || axis >= f->var->naxes || value != value)
        return 0;
    const AuFontAxis *a = &f->var->axes[axis];
    if (value < a->min)
        value = a->min;
    if (value > a->max)
        value = a->max;
    float n = 0;
    if (value < a->def)
        n = a->def > a->min ? (value - a->def) / (a->def - a->min) : 0;
    else if (value > a->def)
        n = a->max > a->def ? (value - a->def) / (a->max - a->def) : 0;
    /* avar: piecewise linear through the axis's (from, to) pairs. */
    uint16_t cnt = f->var->avar_n[axis];
    if (cnt) {
        uint32_t m = f->var->avar_map[axis];
        float prev_from = 0, prev_to = 0;
        bool done = false;
        for (uint32_t i = 0; i < cnt && !done; i++) {
            float from = au_fd_s16(f, m + i * 4) / F2DOT14, to = au_fd_s16(f, m + i * 4 + 2) / F2DOT14;
            if (n == from) {
                n = to;
                done = true;
            } else if (n < from) {
                if (i == 0)
                    n = n + to - from;
                else if (from > prev_from)
                    n = prev_to + (to - prev_to) * (n - prev_from) / (from - prev_from);
                else
                    n = to;
                done = true;
            }
            prev_from = from;
            prev_to = to;
        }
        if (!done)
            n = n + prev_to - prev_from;
    }
    float q = floorf(n * F2DOT14 + 0.5f);
    if (q < -16384)
        q = -16384;
    if (q > 16384)
        q = 16384;
    return (int)q;
}

int au_font_named_instance_count(const AuFontFace *f)
{
    return f->var ? f->var->ninst : 0;
}

bool au_font_named_instance(const AuFontFace *f, int i, char *name, size_t cap, float *coords)
{
    if (!f->var || i < 0 || i >= f->var->ninst)
        return false;
    uint32_t r = f->var->inst + (uint32_t)i * f->var->inst_size;
    if (name && cap && !au_font_name_string(f, au_fd_u16(f, r), name, cap))
        name[0] = 0;
    if (coords)
        for (int k = 0; k < f->var->naxes; k++)
            coords[k] = au_fd_s32(f, r + 4 + (uint32_t)k * 4) / 65536.0f;
    return true;
}

/* ---- Scalars ---- */

/* The scalar of a region for f's coordinates: per axis (start, peak, end)
 * as F2Dot14, at axes (stride 2 bytes between values of one kind). */
static float axis_factor(int coord, int start, int peak, int end)
{
    if (peak == 0 || coord == peak)
        return 1;
    if (start > peak || peak > end)
        return 1; /* invalid region: the axis is ignored */
    if (start < 0 && end > 0)
        return 1;
    if (coord <= start || coord >= end)
        return 0;
    if (coord < peak)
        return (float)(coord - start) / (float)(peak - start);
    return (float)(end - coord) / (float)(end - peak);
}

static float region_scalar(const AuFontFace *f, uint32_t region, int raxes)
{
    float s = 1;
    for (int i = 0; i < raxes; i++) {
        uint32_t r = region + (uint32_t)i * 6;
        int start = au_fd_s16(f, r), peak = au_fd_s16(f, r + 2), end = au_fd_s16(f, r + 4);
        int coord = i < f->var->naxes ? f->coords[i] : 0;
        s *= axis_factor(coord, start, peak, end);
        if (s == 0)
            return 0;
    }
    return s;
}

/* gvar tuple scalar: peak tuple, and optional intermediate start/end. */
static float tuple_scalar(const AuFontFace *f, uint32_t peak, uint32_t start, uint32_t end)
{
    float s = 1;
    for (int i = 0; i < f->var->naxes; i++) {
        int p = au_fd_s16(f, peak + (uint32_t)i * 2);
        if (p == 0)
            continue;
        int c = f->coords[i];
        if (c == p)
            continue;
        if (!start) {
            if (c == 0 || (c < 0 && p > 0) || (c > 0 && p < 0) || (c < 0 ? c < p : c > p))
                return 0;
            s *= (float)c / (float)p;
        } else {
            int a = au_fd_s16(f, start + (uint32_t)i * 2), b = au_fd_s16(f, end + (uint32_t)i * 2);
            s *= axis_factor(c, a, p, b);
            if (s == 0)
                return 0;
        }
    }
    return s;
}

/* ---- Item variation stores (HVAR, MVAR) ---- */

static float ivs_delta(const AuFontFace *f, uint32_t store, uint16_t outer, uint16_t inner)
{
    uint32_t regions = au_fd_add(store, au_fd_u32(f, store + 2));
    uint16_t ndata = au_fd_u16(f, store + 6);
    if (outer >= ndata || outer == 0xFFFF)
        return 0;
    uint32_t data = au_fd_add(store, au_fd_u32(f, store + 8 + (uint32_t)outer * 4));
    uint16_t nitems = au_fd_u16(f, data), words = au_fd_u16(f, data + 2), nreg = au_fd_u16(f, data + 4);
    bool lng = (words & 0x8000) != 0;
    words &= 0x7FFF;
    if (inner >= nitems || words > nreg || nreg > MAX_REGIONS_PER_ROW)
        return 0;
    uint32_t row_size = lng ? (uint32_t)words * 4 + (uint32_t)(nreg - words) * 2
                            : (uint32_t)words * 2 + (uint32_t)(nreg - words);
    uint32_t row = mul_off(data + 6 + (uint32_t)nreg * 2, inner, row_size);
    if (!au_fd_in(f, row, row_size))
        return 0;
    uint16_t raxes = au_fd_u16(f, regions), rcount = au_fd_u16(f, regions + 2);
    if (raxes > 64)
        return 0;
    float sum = 0;
    uint32_t p = row;
    for (uint32_t r = 0; r < nreg; r++) {
        int32_t d;
        if (r < words) {
            d = lng ? au_fd_s32(f, p) : au_fd_s16(f, p);
            p += lng ? 4 : 2;
        } else {
            d = lng ? au_fd_s16(f, p) : (int8_t)au_fd_u8(f, p);
            p += lng ? 2 : 1;
        }
        if (!d)
            continue;
        uint16_t ri = au_fd_u16(f, data + 6 + r * 2);
        if (ri >= rcount)
            continue;
        float s = region_scalar(f, regions + 4 + (uint32_t)ri * raxes * 6, raxes);
        sum += s * (float)d;
    }
    return sum;
}

/* ---- HVAR ---- */

static bool delta_set_index(const AuFontFace *f, uint32_t map, uint32_t idx, uint16_t *outer, uint16_t *inner)
{
    uint8_t fmt = au_fd_u8(f, map), entry = au_fd_u8(f, map + 1);
    uint32_t count, p;
    if (fmt == 0) {
        count = au_fd_u16(f, map + 2);
        p = map + 4;
    } else if (fmt == 1) {
        count = au_fd_u32(f, map + 2);
        p = map + 6;
    } else {
        return false;
    }
    if (!count)
        return false;
    if (idx >= count)
        idx = count - 1;
    uint32_t size = ((entry >> 4) & 3) + 1, bits = (entry & 0x0F) + 1;
    uint32_t at = mul_off(p, idx, size);
    if (!au_fd_in(f, at, size))
        return false;
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++)
        v = (v << 8) | f->data[at + i];
    *outer = (uint16_t)(v >> bits);
    *inner = (uint16_t)(v & ((1u << bits) - 1));
    return true;
}

/* ---- gvar ---- */

typedef struct Rd {
    const AuFontFace *f;
    uint32_t p, end;
    bool bad;
} Rd;

static uint8_t rd_u8(Rd *r)
{
    if (r->p >= r->end || !au_fd_in(r->f, r->p, 1)) {
        r->bad = true;
        return 0;
    }
    return r->f->data[r->p++];
}

static uint16_t rd_u16(Rd *r)
{
    uint16_t hi = rd_u8(r);
    return (uint16_t)((hi << 8) | rd_u8(r));
}

/* Packed point numbers: *all is set for "all points". Returns the count;
 * *pts (au_alloc'd, caller frees) holds them. */
static uint32_t read_points(Rd *r, uint16_t **pts, bool *all)
{
    *pts = NULL;
    *all = false;
    uint32_t n = rd_u8(r);
    if (n & 0x80)
        n = ((n & 0x7F) << 8) | rd_u8(r);
    if (r->bad)
        return 0;
    if (n == 0) {
        *all = true;
        return 0;
    }
    uint16_t *v = au_alloc(n * sizeof *v);
    uint32_t i = 0, cur = 0;
    while (i < n && !r->bad) {
        uint8_t ctl = rd_u8(r);
        uint32_t run = (ctl & 0x7F) + 1u;
        for (uint32_t k = 0; k < run && i < n && !r->bad; k++) {
            cur += (ctl & 0x80) ? rd_u16(r) : rd_u8(r);
            v[i++] = (uint16_t)cur; /* wraps as fontTools does; out-of-range indices are ignored */
        }
    }
    if (r->bad) {
        au_free(v);
        return 0;
    }
    *pts = v;
    return n;
}

/* Packed deltas: n values into out. */
static bool read_deltas(Rd *r, uint32_t n, float *out)
{
    uint32_t i = 0;
    while (i < n && !r->bad) {
        uint8_t ctl = rd_u8(r);
        uint32_t run = (ctl & 0x3F) + 1u;
        for (uint32_t k = 0; k < run && i < n && !r->bad; k++) {
            if (ctl & 0x80)
                out[i++] = 0;
            else if (ctl & 0x40)
                out[i++] = (int16_t)rd_u16(r);
            else
                out[i++] = (int8_t)rd_u8(r);
        }
    }
    return !r->bad;
}

/* Interpolates the untouched points of one contour [s, e] (IUP), per
 * coordinate, from the touched points around them (fontTools' iup). */
static void iup_contour(int s, int e, const float *x, const float *y, const uint8_t *touched, float *dx, float *dy)
{
    int first = -1, count = 0;
    for (int i = s; i <= e; i++)
        if (touched[i]) {
            if (first < 0)
                first = i;
            count++;
        }
    if (count == 0 || count == e - s + 1)
        return;
    if (count == 1) {
        for (int i = s; i <= e; i++)
            if (i != first) {
                dx[i] = dx[first];
                dy[i] = dy[first];
            }
        return;
    }
    int n = e - s + 1;
    int i = first;
    for (int seen = 0; seen < count; seen++) {
        /* The next touched point after i, cyclically. */
        int j = i;
        do
            j = j == e ? s : j + 1;
        while (!touched[j]);
        for (int k = i == e ? s : i + 1; k != j; k = k == e ? s : k + 1) {
            for (int axis = 0; axis < 2; axis++) {
                const float *c = axis ? y : x;
                float *d = axis ? dy : dx;
                float x1 = c[i], x2 = c[j], d1 = d[i], d2 = d[j];
                if (x1 == x2) {
                    d[k] = d1 == d2 ? d1 : 0;
                    continue;
                }
                if (x1 > x2) {
                    float t = x1;
                    x1 = x2;
                    x2 = t;
                    t = d1;
                    d1 = d2;
                    d2 = t;
                }
                float v = c[k];
                d[k] = v <= x1 ? d1 : v >= x2 ? d2 : d1 + (v - x1) * (d2 - d1) / (x2 - x1);
            }
        }
        i = j;
        (void)n;
    }
}

bool au_fontvar_glyph_deltas(const AuFontFace *f, uint16_t g, int n, const uint16_t *ends, int ncont,
                             const float *x, const float *y, float *dx, float *dy)
{
    const AuFontVar *v = f->var;
    if (!v || !v->gvar_ok || !f->varied || g >= v->gvar_glyphs || n <= 0 || n > MAX_GVAR_POINTS)
        return true;
    uint32_t o0, o1;
    if (v->gvar_long) {
        o0 = au_fd_u32(f, v->gvar_offsets + (uint32_t)g * 4);
        o1 = au_fd_u32(f, v->gvar_offsets + (uint32_t)g * 4 + 4);
    } else {
        o0 = (uint32_t)au_fd_u16(f, v->gvar_offsets + (uint32_t)g * 2) * 2;
        o1 = (uint32_t)au_fd_u16(f, v->gvar_offsets + (uint32_t)g * 2 + 2) * 2;
    }
    if (o1 <= o0)
        return true; /* no variation data */
    uint32_t base = au_fd_add(v->glyph_data, o0), len = o1 - o0;
    uint32_t gend = au_fd_add(base, len);
    if (gend > f->gvar + f->gvar_len || len < 4)
        return false;
    uint16_t cnt = au_fd_u16(f, base), data_off = au_fd_u16(f, base + 2);
    uint32_t ntuples = cnt & 0x0FFF;
    if (data_off > len)
        return false;
    Rd sr = {f, base + data_off, gend, false};
    uint16_t *shared_pts = NULL;
    bool shared_all = false;
    uint32_t nshared_pts = 0;
    if (cnt & 0x8000) {
        nshared_pts = read_points(&sr, &shared_pts, &shared_all);
        if (sr.bad)
            return false;
    }
    int nout = ends && ncont > 0 ? ends[ncont - 1] + 1 : 0; /* outline points (IUP) */
    if (nout > n)
        return false;
    float *tx = NULL, *ty = NULL;
    uint8_t *touched = NULL;
    if (nout) {
        tx = au_alloc((size_t)n * 2 * sizeof(float));
        ty = tx + n;
        touched = au_alloc((size_t)n);
    }
    bool ok = true;
    uint32_t work = 0;
    uint32_t hdr = base + 4, hend = base + data_off;
    uint32_t axes2 = (uint32_t)v->naxes * 2;
    for (uint32_t t = 0; t < ntuples && ok; t++) {
        if (hdr + 4 > hend) {
            ok = false;
            break;
        }
        uint16_t size = au_fd_u16(f, hdr), idx = au_fd_u16(f, hdr + 2);
        hdr += 4;
        uint32_t peak, start = 0, end = 0;
        if (idx & 0x8000) {
            peak = hdr;
            hdr += axes2;
        } else {
            if ((idx & 0x0FFF) >= v->nshared) {
                ok = false;
                break;
            }
            peak = v->shared_tuples + (uint32_t)(idx & 0x0FFF) * axes2;
        }
        if (idx & 0x4000) {
            start = hdr;
            end = hdr + axes2;
            hdr += 2 * axes2;
        }
        if (hdr > hend) {
            ok = false;
            break;
        }
        uint32_t tstart = sr.p;
        if (tstart > gend || size > gend - tstart) {
            ok = false;
            break;
        }
        sr.p = tstart + size; /* the next tuple's data */
        float s = tuple_scalar(f, peak, start, end);
        if (s == 0)
            continue;
        Rd r = {f, tstart, tstart + size, false};
        uint16_t *pts = shared_pts;
        bool all = shared_all;
        uint32_t npts = nshared_pts;
        uint16_t *priv = NULL;
        if (idx & 0x2000) {
            npts = read_points(&r, &priv, &all);
            pts = priv;
            if (r.bad) {
                ok = false;
                break;
            }
        } else if (!(cnt & 0x8000)) {
            all = true; /* no point numbers at all: every point */
        }
        uint32_t m = all ? (uint32_t)n : npts;
        work += m * 2;
        if (work > MAX_DELTA_WORK) {
            au_free(priv);
            ok = false;
            break;
        }
        float *ax = au_alloc((size_t)(m ? m : 1) * 2 * sizeof(float));
        float *ay = ax + (m ? m : 1);
        if (!read_deltas(&r, m, ax) || !read_deltas(&r, m, ay)) {
            au_free(ax);
            au_free(priv);
            ok = false;
            break;
        }
        if (all) {
            for (int i = 0; i < n; i++) {
                dx[i] += s * ax[i];
                dy[i] += s * ay[i];
            }
        } else if (!nout) {
            for (uint32_t i = 0; i < m; i++)
                if (pts[i] < n) {
                    dx[pts[i]] += s * ax[i];
                    dy[pts[i]] += s * ay[i];
                }
        } else {
            memset(tx, 0, (size_t)n * 2 * sizeof(float));
            memset(touched, 0, (size_t)n);
            for (uint32_t i = 0; i < m; i++)
                if (pts[i] < n) {
                    tx[pts[i]] = ax[i];
                    ty[pts[i]] = ay[i];
                    touched[pts[i]] = 1;
                }
            int cs = 0;
            for (int c = 0; c < ncont; c++) {
                int ce = ends[c];
                if (ce >= cs)
                    iup_contour(cs, ce, x, y, touched, tx, ty);
                cs = ce + 1;
            }
            for (int i = 0; i < n; i++) {
                dx[i] += s * tx[i];
                dy[i] += s * ty[i];
            }
        }
        au_free(ax);
        au_free(priv);
    }
    au_free(shared_pts);
    au_free(tx);
    au_free(touched);
    return ok;
}

float au_fontvar_advance(const AuFontFace *f, uint16_t g, int base)
{
    const AuFontVar *v = f->var;
    if (!v || !f->varied)
        return (float)base;
    if (v->hvar_store) {
        uint16_t outer = 0, inner = g;
        if (v->hvar_map && !delta_set_index(f, v->hvar_map, g, &outer, &inner))
            return (float)base;
        return base + ivs_delta(f, v->hvar_store, outer, inner);
    }
    if (!v->gvar_ok)
        return (float)base;
    int np = 0;
    bool composite = false;
    if (!au_font_glyph_point_count(f, g, &np, &composite))
        return (float)base;
    int n = np + 4;
    float *d = au_calloc((size_t)n * 2 * sizeof(float));
    float adv = (float)base;
    if (au_fontvar_glyph_deltas(f, g, n, NULL, 0, NULL, NULL, d, d + n))
        adv += d[np + 1] - d[np];
    au_free(d);
    return adv;
}

/* ---- MVAR ---- */

static int16_t clamp16(float v)
{
    v = floorf(v + 0.5f);
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void au_fontvar_apply_metrics(AuFontFace *f)
{
    const AuFontVar *v = f->var;
    if (!v || !v->mvar_store || !f->varied)
        return;
    for (uint32_t i = 0; i < v->mvar_nrec; i++) {
        uint32_t r = v->mvar_recs + i * v->mvar_recsize;
        uint32_t tag = au_fd_u32(f, r);
        float d = ivs_delta(f, v->mvar_store, au_fd_u16(f, r + 4), au_fd_u16(f, r + 6));
        if (d == 0)
            continue;
        int16_t *dst = NULL;
        switch (tag) {
        case AU_TAG('h', 'a', 's', 'c'): dst = f->metric_src == 1 ? &f->ascender : NULL; break;
        case AU_TAG('h', 'd', 's', 'c'): dst = f->metric_src == 1 ? &f->descender : NULL; break;
        case AU_TAG('h', 'l', 'g', 'c'): dst = f->metric_src == 1 ? &f->line_gap : NULL; break;
        case AU_TAG('h', 'c', 'l', 'a'): dst = f->metric_src == 2 ? &f->ascender : NULL; break;
        case AU_TAG('h', 'c', 'l', 'd'):
            if (f->metric_src == 2)
                f->descender = clamp16(f->descender - d); /* usWinDescent is positive */
            break;
        case AU_TAG('x', 'h', 'g', 't'): dst = &f->x_height; break;
        case AU_TAG('c', 'p', 'h', 't'): dst = &f->cap_height; break;
        case AU_TAG('u', 'n', 'd', 'o'): dst = &f->underline_pos; break;
        case AU_TAG('u', 'n', 'd', 's'): dst = &f->underline_thickness; break;
        case AU_TAG('s', 't', 'r', 'o'): dst = &f->strike_pos; break;
        case AU_TAG('s', 't', 'r', 's'): dst = &f->strike_thickness; break;
        }
        if (dst)
            *dst = clamp16(*dst + d);
    }
}

/* ---- STAT naming ---- */

/* Appends the STAT name of axis value records matching the coordinates
 * (user values), skipping elidable ones; false when STAT is unusable. */
static bool stat_name(const AuFontFace *f, const float *user, char *out, size_t cap)
{
    uint32_t b = f->stat;
    if (f->stat_len < 18 || au_fd_u16(f, b) != 1)
        return false;
    uint16_t axis_size = au_fd_u16(f, b + 4), naxes = au_fd_u16(f, b + 6);
    uint32_t axes = au_fd_add(b, au_fd_u32(f, b + 8));
    uint16_t nvalues = au_fd_u16(f, b + 12);
    uint32_t values = au_fd_add(b, au_fd_u32(f, b + 14));
    uint16_t elided = au_fd_u16(f, b + 2) >= 1 && f->stat_len >= 20 ? au_fd_u16(f, b + 18) : 0;
    if (axis_size < 8 || nvalues > 1024)
        return false;
    size_t n = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < nvalues; i++) {
        uint32_t r = au_fd_add(values, au_fd_u16(f, values + i * 2));
        uint16_t fmt = au_fd_u16(f, r), axis = au_fd_u16(f, r + 2), flags = au_fd_u16(f, r + 4);
        uint16_t name = au_fd_u16(f, r + 6);
        if ((fmt != 1 && fmt != 2 && fmt != 3) || axis >= naxes || (flags & 2))
            continue;
        uint32_t tag = au_fd_u32(f, axes + (uint32_t)axis * axis_size);
        int k = au_font_axis_find(f, tag);
        if (k < 0)
            continue;
        float u = user[k];
        bool match;
        if (fmt == 2)
            match = u >= au_fd_s32(f, r + 12) / 65536.0f && u <= au_fd_s32(f, r + 16) / 65536.0f;
        else
            match = fabsf(au_fd_s32(f, r + 8) / 65536.0f - u) < 0.5f;
        if (!match)
            continue;
        char tmp[64];
        if (!au_font_name_string(f, name, tmp, sizeof tmp))
            continue;
        size_t tl = strlen(tmp);
        if (n + tl + 2 > cap)
            break;
        if (n)
            out[n++] = ' ';
        memcpy(out + n, tmp, tl + 1);
        n += tl;
    }
    if (!n && !au_font_name_string(f, elided, out, cap))
        return false;
    return true;
}

/* ---- Instances ---- */

AuFontFace *au_font_face_instance(const AuFontFace *f, const AuFontVariation *vars, size_t n)
{
    AuFontFace *v = au_font_face_synthesize(f, 0);
    if (!f->var)
        return v;
    float user[AU_FONT_MAX_AXES];
    for (int i = 0; i < f->var->naxes; i++)
        user[i] = f->var->axes[i].def;
    for (size_t k = 0; k < n; k++) {
        int i = au_font_axis_find(f, vars[k].tag);
        if (i < 0 || vars[k].value != vars[k].value)
            continue;
        const AuFontAxis *a = &f->var->axes[i];
        float u = vars[k].value < a->min ? a->min : vars[k].value > a->max ? a->max : vars[k].value;
        user[i] = u;
        v->coords[i] = (int16_t)au_font_normalize(f, i, u);
    }
    v->varied = false;
    for (int i = 0; i < f->var->naxes; i++)
        if (v->coords[i])
            v->varied = true;
    /* Metrics: the default instance's, then MVAR at the new
     * coordinates. */
    const int16_t *dm = f->var->def_metrics;
    v->ascender = dm[0], v->descender = dm[1], v->line_gap = dm[2];
    v->x_height = dm[3], v->cap_height = dm[4];
    v->underline_pos = dm[5], v->underline_thickness = dm[6];
    v->strike_pos = dm[7], v->strike_thickness = dm[8];
    au_fontvar_apply_metrics(v);
    int w = au_font_axis_find(f, AU_TAG('w', 'g', 'h', 't'));
    if (w >= 0) {
        int wt = au_f2i(floorf(user[w] + 0.5f));
        v->weight = (uint16_t)(wt < 1 ? 1 : wt > 1000 ? 1000 : wt);
    }
    int it = au_font_axis_find(f, AU_TAG('i', 't', 'a', 'l'));
    int sl = au_font_axis_find(f, AU_TAG('s', 'l', 'n', 't'));
    if (it >= 0)
        v->italic = user[it] >= 0.5f;
    else if (sl >= 0)
        v->italic = user[sl] < -0.5f;
    /* The name of the instance: a named instance at these coordinates,
     * else STAT's names for the axis values. */
    bool named = false;
    for (int i = 0; i < f->var->ninst && !named; i++) {
        float c[AU_FONT_MAX_AXES];
        char nm[64];
        if (!au_font_named_instance(f, i, nm, sizeof nm, c) || !nm[0])
            continue;
        bool same = true;
        for (int k = 0; k < f->var->naxes; k++)
            if (fabsf(c[k] - user[k]) > 0.01f)
                same = false;
        if (same) {
            memcpy(v->subfamily, nm, sizeof nm);
            named = true;
        }
    }
    if (!named) {
        char nm[64];
        if (stat_name(f, user, nm, sizeof nm)) {
            memcpy(v->subfamily, nm, sizeof nm);
        }
    }
    return v;
}
