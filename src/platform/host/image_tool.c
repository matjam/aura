/* aura-host image FILE [-o out.png] [--scale N] [--target WxH] [--max-bytes N]
 *                      [--chunk N] [--frames] [--repeat N] [--favicon SIZE]
 *
 * Decodes one image file with src/image and prints its format, sizes,
 * frames, decode time and memory. -o writes the decoded canvas (the
 * first frame, or the last with --frames) as a PNG. --scale N asks for
 * 1/N of the intrinsic size; --target asks for at least WxH. --chunk N
 * feeds the decoder N bytes at a time (as a network would). --repeat
 * times the decode N times and reports the best. --favicon SIZE decodes
 * as a favicon of SIZE x SIZE pixels. */
#include "image/image.h"
#include "png.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = n > 0 ? (size_t)n : 0;
    return b;
}

static int image_usage(void)
{
    fprintf(stderr, "usage: aura-host image FILE [-o out.png] [--scale N] [--target WxH] [--max-bytes N]\n"
                    "                        [--chunk N] [--frames] [--repeat N] [--favicon SIZE]\n");
    return 2;
}

int au_host_image_main(int argc, char **argv)
{
    if (argc < 3)
        return image_usage();
    const char *path = argv[2], *out = NULL;
    int scale = 1, repeat = 1, favicon = 0;
    size_t chunk = 0;
    bool frames = false;
    AuImageOptions opt;
    memset(&opt, 0, sizeof opt);
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "--scale") && i + 1 < argc)
            scale = atoi(argv[++i]);
        else if (!strcmp(a, "--target") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &opt.target_w, &opt.target_h);
        else if (!strcmp(a, "--max-bytes") && i + 1 < argc)
            opt.max_bytes = (size_t)atol(argv[++i]);
        else if (!strcmp(a, "--chunk") && i + 1 < argc)
            chunk = (size_t)atol(argv[++i]);
        else if (!strcmp(a, "--repeat") && i + 1 < argc)
            repeat = atoi(argv[++i]);
        else if (!strcmp(a, "--favicon") && i + 1 < argc)
            favicon = atoi(argv[++i]);
        else if (!strcmp(a, "--frames"))
            frames = true;
        else
            return image_usage();
    }
    if (scale < 1 || repeat < 1)
        return image_usage();
    size_t len;
    uint8_t *data = read_all(path, &len);
    if (!data) {
        fprintf(stderr, "aura-host: cannot read %s\n", path);
        return 1;
    }
    if (favicon > 0) {
        double t0 = ms_now();
        AuSurface *s = au_image_decode_favicon(data, len, favicon);
        double t1 = ms_now();
        if (!s) {
            printf("%s: not an image\n", path);
            free(data);
            return 1;
        }
        printf("%s: favicon %dx%d in %.2f ms\n", path, s->width, s->height, t1 - t0);
        if (out && !au_png_write(out, s))
            fprintf(stderr, "aura-host: cannot write %s\n", out);
        au_surface_free(s);
        free(data);
        return 0;
    }
    AuImageInfo probe;
    if (scale > 1 && au_image_probe(data, len, &probe)) {
        opt.target_w = (probe.width + scale - 1) / scale;
        opt.target_h = (probe.height + scale - 1) / scale;
    }
    double best = 1e30, first_ms = 0;
    AuSurface *keep = NULL;
    AuImageInfo info;
    memset(&info, 0, sizeof info);
    const char *err = "";
    AuImageStatus last = AU_IMAGE_MORE;
    for (int r = 0; r < repeat; r++) {
        AuImageDecoder *d = au_image_decoder_new(&opt);
        double t0 = ms_now(), tf = 0;
        size_t pos = 0;
        AuImageStatus st = AU_IMAGE_MORE;
        uint32_t nframes = 0;
        for (;;) {
            size_t c = chunk ? chunk : len - pos;
            if (c > len - pos)
                c = len - pos;
            const uint8_t *p = data + pos;
            size_t n = c;
            st = au_image_decode(d, &p, &n, pos + c == len);
            pos += c - n;
            if (st == AU_IMAGE_FRAME) {
                if (!nframes++)
                    tf = ms_now() - t0;
                if (frames)
                    continue;
                break;
            }
            if (st != AU_IMAGE_MORE)
                break;
        }
        double t = ms_now() - t0;
        if (t < best)
            best = t;
        if (r == 0) {
            first_ms = tf;
            info = *au_image_info(d);
            err = st == AU_IMAGE_ERROR ? au_image_error(d) : "";
            static char errbuf[96];
            snprintf(errbuf, sizeof errbuf, "%s", err);
            err = errbuf;
            last = st;
            if (au_image_canvas(d))
                keep = au_image_take_canvas(d);
        }
        au_image_decoder_free(d);
    }
    printf("%s: %s, %d bytes\n", path, au_image_format_name(info.format), (int)len);
    if (info.ready) {
        printf("  size %dx%d, decoded %dx%d (1/%d)%s%s%s\n", info.width, info.height, info.out_w, info.out_h,
               info.scale_denom, info.has_alpha ? ", alpha" : "", keep && keep->opaque ? ", opaque" : "",
               info.orientation != 1 ? ", EXIF orientation applied" : "");
        if (info.animated || info.frame_count > 1)
            printf("  animated: %u frames decoded, loop %d (0 = forever), last delay %u ms\n", info.frame_count,
                   info.loop_count, info.frame_delay_ms);
        double mp = (double)info.width * info.height / 1e6;
        printf("  decode %.2f ms (best of %d; first frame at %.2f ms), %.1f MP/s of source\n", best, repeat, first_ms,
               best > 0 ? mp / (best / 1000.0) : 0);
        printf("  memory: peak %.1f KB (canvas %.1f KB)\n", info.peak_bytes / 1024.0,
               (double)info.out_w * info.out_h * 4 / 1024.0);
    }
    if (last == AU_IMAGE_ERROR)
        printf("  error: %s%s\n", err, info.partial ? " (partial image kept)" : "");
    int rc = info.ready && (last != AU_IMAGE_ERROR || info.partial) ? 0 : 1;
    if (out && keep) {
        if (!au_png_write(out, keep)) {
            fprintf(stderr, "aura-host: cannot write %s\n", out);
            rc = 1;
        }
    }
    au_surface_free(keep);
    free(data);
    return rc;
}
