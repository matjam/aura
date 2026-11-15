/* GIF87a and GIF89a: LZW, global and local palettes, interlacing,
 * transparency, and animation with the disposal methods, frame delays
 * and the NETSCAPE2.0 loop count.
 *
 * A byte-level state machine; LZW codes are decoded as their bytes
 * arrive and rows are emitted as they complete, so a large image fills
 * in while it loads. Frames are composited on a canvas of the logical
 * screen size (the first frame's extent when the screen is 0 x 0),
 * clipped to it. As browsers do, disposal to background clears to
 * transparent (the background colour index is ignored), and a frame
 * whose data ends early is shown as far as it got.
 *
 * The first frame of an interlaced image without transparency fills in
 * progressively: each pass's row is repeated over the rows below it
 * until later passes replace them. */
#include "image_int.h"

enum {
    G_HEADER,   /* 13 bytes: signature and logical screen descriptor */
    G_GCT,      /* the global colour table */
    G_BLOCK,    /* the introducer of the next block */
    G_EXT_LABEL,
    G_EXT_LEN,  /* sub-block length of an extension */
    G_EXT_DATA, /* sub-block bytes */
    G_DESC,     /* 9 bytes of image descriptor */
    G_LCT,
    G_MINCODE,
    G_DATA_LEN,
    G_DATA,
    G_END
};

#define LZW_MAX 4096

typedef struct {
    int state;
    AuImgHold hold;
    int sw, sh; /* logical screen */
    uint32_t gct[256], lct[256];
    int ngct, nlct;
    bool has_gct;
    int ext_label;
    size_t sub_left;
    uint8_t ext_buf[16];
    size_t ext_n;
    bool ext_netscape, ext_first_block;

    /* The graphic control extension for the next image. */
    bool gce_valid;
    int gce_dispose;
    bool gce_trans;
    int gce_index;
    uint32_t gce_delay;

    /* The current frame. */
    int fx, fy, fw, fh;
    bool interlace;
    bool trans;
    int tindex;
    int dispose;
    uint32_t delay;
    const uint32_t *pal;
    int npal;
    int row, pass;   /* the row being filled (frame coordinates), interlace pass */
    int col;
    int rows_done;
    bool fill;       /* progressive fill of interlaced rows */
    uint32_t *line;  /* fw pixels */
    size_t line_cap;

    /* The previous frame, for its disposal. */
    bool prev_valid;
    int px, py, pw, ph, pdispose;
    uint32_t *saved;

    /* LZW. */
    int min_code, code_size, clear, eoi, next, old;
    uint32_t bits;
    int nbits;
    bool lzw_end; /* end code seen, or the frame is complete */
    uint16_t prefix[LZW_MAX];
    uint8_t suffix[LZW_MAX];
    uint8_t first[LZW_MAX]; /* first byte of each code's string */
    uint8_t stack[LZW_MAX];

    bool begun;
    uint32_t frames;
    int loop;
} Gif;

static void *create(AuImageDecoder *d)
{
    Gif *g = au_calloc(sizeof *g);
    g->state = G_HEADER;
    g->loop = 1; /* without a NETSCAPE2.0 extension: play once */
    au_img_mem(d, (ptrdiff_t)sizeof *g);
    return g;
}

static void destroy(void *st)
{
    Gif *g = st;
    au_img_hold_free(&g->hold);
    au_free(g->line);
    au_free(g->saved);
    au_free(g);
}

static void read_palette(uint32_t *pal, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        pal[i] = 0xFF000000u | (uint32_t)p[3 * i] << 16 | (uint32_t)p[3 * i + 1] << 8 | p[3 * i + 2];
}

static bool ensure_header(Gif *g, AuImageDecoder *d)
{
    if (g->begun)
        return true;
    if (!au_img_header(d, g->sw, g->sh, 1, true))
        return false;
    if (!au_img_begin(d, g->sw, g->sh, au_img_scale(d)))
        return false;
    au_img_set_animation(d, false, g->loop);
    g->begun = true;
    return true;
}

/* ---- Rows ---- */

static const uint8_t ilace_start[4] = {0, 4, 2, 1}, ilace_step[4] = {8, 8, 4, 2};

static void emit_line(Gif *g, AuImageDecoder *d)
{
    int y = g->row;
    int blend = g->trans ? AU_IMG_BLEND_KEY : AU_IMG_BLEND_SRC;
    au_img_frame_row(d, g->fx, g->fy + y, g->fw, g->line, blend);
    if (g->fill && g->interlace) {
        /* Repeat over the rows this pass stands for. */
        int span = g->pass == 0 ? 8 : g->pass == 1 ? 4 : g->pass == 2 ? 2 : 1;
        for (int k = 1; k < span && y + k < g->fh; k++)
            au_img_frame_row(d, g->fx, g->fy + y + k, g->fw, g->line, AU_IMG_BLEND_SRC);
    }
    g->rows_done++;
    if (g->interlace) {
        g->row += ilace_step[g->pass];
        while (g->row >= g->fh && g->pass < 3) {
            g->pass++;
            g->row = ilace_start[g->pass];
        }
    } else {
        g->row++;
    }
}

static inline void put_pixel(Gif *g, AuImageDecoder *d, int index)
{
    if (g->rows_done >= g->fh)
        return;
    uint32_t c;
    if (g->trans && index == g->tindex)
        c = 0;
    else if (index < g->npal)
        c = g->pal[index];
    else
        c = 0xFF000000u; /* outside the palette: black */
    g->line[g->col++] = c;
    if (g->col == g->fw) {
        emit_line(g, d);
        g->col = 0;
    }
}

/* ---- LZW ---- */

static void lzw_reset(Gif *g)
{
    g->code_size = g->min_code + 1;
    g->next = g->eoi + 1;
    g->old = -1;
}

static void lzw_start(Gif *g)
{
    g->clear = 1 << g->min_code;
    g->eoi = g->clear + 1;
    for (int i = 0; i < g->clear; i++) {
        g->prefix[i] = 0;
        g->suffix[i] = (uint8_t)i;
        g->first[i] = (uint8_t)i;
    }
    g->bits = 0;
    g->nbits = 0;
    g->lzw_end = false;
    lzw_reset(g);
}

/* Decodes the codes in data. */
static void lzw_feed(Gif *g, AuImageDecoder *d, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n && !g->lzw_end; i++) {
        g->bits |= (uint32_t)data[i] << g->nbits;
        g->nbits += 8;
        while (g->nbits >= g->code_size && !g->lzw_end) {
            int code = (int)(g->bits & ((1u << g->code_size) - 1));
            g->bits >>= g->code_size;
            g->nbits -= g->code_size;
            if (code == g->clear) {
                lzw_reset(g);
                continue;
            }
            if (code == g->eoi) {
                g->lzw_end = true;
                break;
            }
            int sp = 0, cur;
            if (g->old < 0) {
                /* The first code after a clear must be a literal. */
                if (code >= g->clear) {
                    g->lzw_end = true; /* corrupt: show what there is */
                    break;
                }
                put_pixel(g, d, code);
                g->old = code;
                continue;
            }
            if (code < g->next) {
                cur = code;
            } else if (code == g->next && g->next < LZW_MAX) {
                /* KwKwK: the previous string plus its own first byte. */
                g->stack[sp++] = g->first[g->old];
                cur = g->old;
            } else {
                g->lzw_end = true; /* a code not yet defined: corrupt */
                break;
            }
            /* Unwind cur onto the stack (at most LZW_MAX entries: each
             * step moves to a strictly smaller code). */
            while (cur >= g->clear && sp < LZW_MAX - 1) {
                g->stack[sp++] = g->suffix[cur];
                cur = g->prefix[cur];
            }
            g->stack[sp++] = (uint8_t)cur;
            uint8_t firstb = (uint8_t)cur;
            if (g->next < LZW_MAX) {
                g->prefix[g->next] = (uint16_t)g->old;
                g->suffix[g->next] = firstb;
                g->first[g->next] = g->first[g->old];
                g->next++;
                if (g->next == (1 << g->code_size) && g->code_size < 12)
                    g->code_size++;
            }
            while (sp > 0)
                put_pixel(g, d, g->stack[--sp]);
            g->old = code;
            if (g->rows_done >= g->fh)
                g->lzw_end = true; /* the frame is complete */
        }
    }
}

/* ---- Frames ---- */

static bool begin_frame(Gif *g, AuImageDecoder *d, const uint8_t *desc)
{
    int x = (int)au_img_le16(desc), y = (int)au_img_le16(desc + 2);
    int w = (int)au_img_le16(desc + 4), h = (int)au_img_le16(desc + 6);
    if (g->sw == 0 || g->sh == 0) {
        /* No logical screen: the first frame's extent. */
        g->sw = x + w;
        g->sh = y + h;
    }
    if (!ensure_header(g, d))
        return false;
    if (g->frames > 0)
        au_img_set_animation(d, true, g->loop);
    g->fx = x;
    g->fy = y;
    g->fw = w;
    g->fh = h;
    g->interlace = (desc[8] & 0x40) != 0;
    g->trans = g->gce_valid && g->gce_trans;
    g->tindex = g->gce_index;
    g->dispose = g->gce_valid ? g->gce_dispose : 0;
    g->delay = g->gce_valid ? g->gce_delay : 0;
    g->gce_valid = false;
    g->row = 0;
    g->pass = 0;
    g->col = 0;
    g->rows_done = 0;
    g->fill = g->frames == 0 && g->interlace && !g->trans;
    if (w > 0 && (size_t)w > g->line_cap) {
        au_img_mem(d, (ptrdiff_t)(((size_t)w - g->line_cap) * 4));
        g->line = au_realloc(g->line, (size_t)w * 4);
        g->line_cap = (size_t)w;
    }
    /* Dispose of the previous frame, then save under this one when it
     * is to be restored. */
    if (g->prev_valid) {
        if (g->pdispose == 2)
            au_img_clear_rect(d, g->px, g->py, g->pw, g->ph);
        else if (g->pdispose == 3) {
            au_img_restore_rect(d, g->saved, g->px, g->py, g->pw, g->ph);
            g->saved = NULL;
        }
        g->prev_valid = false;
    }
    if (g->dispose == 3) {
        au_free(g->saved);
        g->saved = au_img_save_rect(d, x, y, w, h);
    }
    return true;
}

static AuImageStatus end_frame(Gif *g, AuImageDecoder *d)
{
    g->prev_valid = true;
    g->px = g->fx;
    g->py = g->fy;
    g->pw = g->fw;
    g->ph = g->fh;
    g->pdispose = g->dispose;
    g->frames++;
    au_img_frame_done(d, g->delay);
    return AU_IMAGE_FRAME;
}

/* ---- Blocks ---- */

static void ext_sub_block(Gif *g, AuImageDecoder *d)
{
    /* Called with a complete sub-block of at most 16 bytes in ext_buf. */
    if (g->ext_label == 0xF9 && g->ext_first_block && g->ext_n >= 4) {
        uint8_t pk = g->ext_buf[0];
        g->gce_valid = true;
        g->gce_dispose = (pk >> 2) & 7;
        g->gce_trans = (pk & 1) != 0;
        g->gce_delay = au_img_le16(g->ext_buf + 1) * 10u;
        g->gce_index = g->ext_buf[3];
    } else if (g->ext_label == 0xFF && g->ext_first_block) {
        g->ext_netscape = g->ext_n == 11 && (!memcmp(g->ext_buf, "NETSCAPE2.0", 11) || !memcmp(g->ext_buf, "ANIMEXTS1.0", 11));
    } else if (g->ext_label == 0xFF && g->ext_netscape && g->ext_n >= 3 && g->ext_buf[0] == 1) {
        uint32_t n = au_img_le16(g->ext_buf + 1);
        g->loop = n == 0 ? 0 : (int)n + 1;
        if (g->begun)
            au_img_set_animation(d, d->info.animated, g->loop);
    }
    g->ext_first_block = false;
}

static AuImageStatus run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    Gif *g = st;
    (void)final;
    for (;;) {
        if (g->state == G_END)
            return AU_IMAGE_DONE;
        if (!*len)
            return AU_IMAGE_MORE;
        switch (g->state) {
        case G_HEADER: {
            if (!au_img_hold(&g->hold, 13, in, len))
                return AU_IMAGE_MORE;
            const uint8_t *h = g->hold.buf;
            if (memcmp(h, "GIF87a", 6) && memcmp(h, "GIF89a", 6))
                return au_img_fail(d, "not a GIF");
            g->sw = (int)au_img_le16(h + 6);
            g->sh = (int)au_img_le16(h + 8);
            g->has_gct = (h[10] & 0x80) != 0;
            g->ngct = 2 << (h[10] & 7);
            g->hold.len = 0;
            if (g->sw > 0 && g->sh > 0 && !ensure_header(g, d))
                return AU_IMAGE_ERROR;
            g->state = g->has_gct ? G_GCT : G_BLOCK;
            break;
        }
        case G_GCT:
            if (!au_img_hold(&g->hold, (size_t)g->ngct * 3, in, len))
                return AU_IMAGE_MORE;
            read_palette(g->gct, g->hold.buf, g->ngct);
            g->hold.len = 0;
            g->state = G_BLOCK;
            break;
        case G_BLOCK: {
            uint8_t b = **in;
            (*in)++;
            (*len)--;
            if (b == 0x21)
                g->state = G_EXT_LABEL;
            else if (b == 0x2C)
                g->state = G_DESC;
            else if (b == 0x3B) {
                g->state = G_END;
                if (!g->frames)
                    return au_img_fail(d, "no image");
                return AU_IMAGE_DONE;
            } else if (b == 0x00) {
                /* Stray terminators between blocks: tolerated. */
            } else {
                if (g->frames)
                    return g->state = G_END, AU_IMAGE_DONE; /* junk after frames: stop */
                return au_img_fail(d, "bad GIF block");
            }
            break;
        }
        case G_EXT_LABEL:
            g->ext_label = **in;
            (*in)++;
            (*len)--;
            g->ext_first_block = true;
            g->ext_netscape = false;
            g->state = G_EXT_LEN;
            break;
        case G_EXT_LEN:
            g->sub_left = **in;
            (*in)++;
            (*len)--;
            g->ext_n = 0;
            g->state = g->sub_left ? G_EXT_DATA : G_BLOCK;
            break;
        case G_EXT_DATA: {
            size_t n = g->sub_left < *len ? g->sub_left : *len;
            for (size_t i = 0; i < n; i++)
                if (g->ext_n < sizeof g->ext_buf)
                    g->ext_buf[g->ext_n++] = (*in)[i];
            *in += n;
            *len -= n;
            g->sub_left -= n;
            if (!g->sub_left) {
                ext_sub_block(g, d);
                g->state = G_EXT_LEN;
            }
            break;
        }
        case G_DESC: {
            if (!au_img_hold(&g->hold, 9, in, len))
                return AU_IMAGE_MORE;
            uint8_t desc[9];
            memcpy(desc, g->hold.buf, 9);
            g->hold.len = 0;
            if (!begin_frame(g, d, desc))
                return AU_IMAGE_ERROR;
            if (desc[8] & 0x80) {
                g->nlct = 2 << (desc[8] & 7);
                g->state = G_LCT;
            } else {
                g->pal = g->gct;
                g->npal = g->has_gct ? g->ngct : 0;
                g->state = G_MINCODE;
            }
            break;
        }
        case G_LCT:
            if (!au_img_hold(&g->hold, (size_t)g->nlct * 3, in, len))
                return AU_IMAGE_MORE;
            read_palette(g->lct, g->hold.buf, g->nlct);
            g->hold.len = 0;
            g->pal = g->lct;
            g->npal = g->nlct;
            g->state = G_MINCODE;
            break;
        case G_MINCODE: {
            int mc = **in;
            (*in)++;
            (*len)--;
            if (mc < 1 || mc > 11)
                return au_img_fail(d, "bad LZW code size");
            g->min_code = mc;
            lzw_start(g);
            if (g->fw == 0 || g->fh == 0)
                g->lzw_end = true; /* an empty frame: skip its data */
            g->state = G_DATA_LEN;
            break;
        }
        case G_DATA_LEN:
            g->sub_left = **in;
            (*in)++;
            (*len)--;
            if (!g->sub_left) {
                /* End of the frame's data: show it as far as it got. */
                g->state = G_BLOCK;
                return end_frame(g, d);
            }
            g->state = G_DATA;
            break;
        case G_DATA: {
            size_t n = g->sub_left < *len ? g->sub_left : *len;
            if (!g->lzw_end)
                lzw_feed(g, d, *in, n);
            *in += n;
            *len -= n;
            g->sub_left -= n;
            if (!g->sub_left)
                g->state = G_DATA_LEN;
            break;
        }
        default:
            return au_img_fail(d, "bad state");
        }
    }
}

const AuImgCodec au_img_gif_codec = {create, run, destroy};
