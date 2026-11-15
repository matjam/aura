/* The Mac OS 9 platform: time, files and fonts. The window functions are
 * in main.c, which owns the window.
 *
 * Fonts: Aura bundles none. The fonts in the System Folder's Fonts folder
 * are font suitcases that the system opens at startup and keeps in the
 * resource chain, so GetIndResource('FOND') walks every installed family.
 * A family's FOND lists its faces; an entry with point size 0 is an
 * outline face, and its resource ID names the 'sfnt' resource (TrueType
 * data, the same bytes as a .ttf) in the same file. Faces are registered
 * by name and style without reading them; the 'sfnt' is read into memory
 * the first time layout picks the face. If the chain shows no FONDs (a
 * system that does not keep suspended fonts in the chain), the Fonts
 * folder's files are opened and searched directly. */
#include <Multiverse.h>

#include <string.h>

#include "platform/platform.h"

/* ---- Time ---- */

/* Microseconds is in InterfaceLib; the Multiversal headers leave it out. */
typedef struct {
    uint32_t hi, lo;
} AuUnsignedWide;
extern pascal void Microseconds(AuUnsignedWide *us);

uint32_t au_platform_time_ms(void)
{
    AuUnsignedWide us;
    Microseconds(&us);
    /* (hi * 2^32 + lo) / 1000, kept in 32 bits: the result wraps like a
     * millisecond counter should. */
    uint64_t v = ((uint64_t)us.hi << 32) | us.lo;
    return (uint32_t)(v / 1000u);
}

/* ---- Files ---- */

bool au_platform_read_file(const char *path, char **data, size_t *len)
{
    Str255 p;
    size_t n = strlen(path);
    if (n > 255)
        return false;
    p[0] = (unsigned char)n;
    memcpy(p + 1, path, n);
    FSSpec spec;
    if (FSMakeFSSpec(0, 0, p, &spec) != noErr)
        return false;
    short ref;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr)
        return false;
    long eof = 0;
    bool ok = false;
    if (GetEOF(ref, &eof) == noErr && eof >= 0) {
        char *buf = au_alloc((size_t)eof + 1);
        long count = eof;
        if (FSRead(ref, &count, buf) == noErr && count == eof) {
            buf[eof] = 0;
            *data = buf;
            *len = (size_t)eof;
            ok = true;
        } else {
            au_free(buf);
        }
    }
    FSClose(ref);
    return ok;
}

/* ---- Fonts ---- */

typedef struct FontRef {
    short file; /* resource file refnum */
    short id;   /* 'sfnt' resource ID */
    Handle mem; /* the data once loaded (locked temporary memory or NULL) */
    Ptr ptr;    /* the data when it went into the application heap */
} FontRef;

static bool load_sfnt(void *ctx, const void **data, size_t *len)
{
    FontRef *f = ctx;
    short saved = CurResFile();
    UseResFile(f->file);
    SetResLoad(false);
    Handle h = Get1Resource('sfnt', f->id);
    SetResLoad(true);
    UseResFile(saved);
    if (!h)
        return false;
    long size = GetResourceSizeOnDisk(h);
    if (size <= 0)
        return false;
    /* Font data lives in temporary memory when there is room, so a dozen
     * faces do not crowd the application heap; else in the heap. */
    OSErr err;
    Ptr buf = NULL;
    Handle th = TempNewHandle(size, &err);
    if (th && err == noErr) {
        HLock(th);
        buf = *th;
    } else {
        th = NULL;
        buf = NewPtr(size);
        if (!buf)
            return false;
    }
    ReadPartialResource(h, 0, buf, size);
    if (ResError() != noErr) {
        if (th)
            DisposeHandle(th);
        else
            DisposePtr(buf);
        return false;
    }
    f->mem = th;
    f->ptr = th ? NULL : buf;
    *data = buf;
    *len = (size_t)size;
    return true;
}

/* Mac Roman to UTF-8 for family names (the high half, as in sfnt.c). */
static const uint16_t mac_roman_high[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

static void pstr_to_utf8(const unsigned char *p, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 1; i <= p[0]; i++) {
        uint32_t cp = p[i] < 128 ? p[i] : mac_roman_high[p[i] - 128];
        char tmp[4];
        int k = au_utf8_encode(cp, tmp);
        if (n + (size_t)k + 1 > cap)
            break;
        memcpy(out + n, tmp, (size_t)k);
        n += (size_t)k;
    }
    out[n] = 0;
}

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Registers the outline faces of one FOND. Returns how many. */
static int add_fond(AuFontSet *set, Handle fond)
{
    short id;
    ResType type;
    Str255 name;
    GetResInfo(fond, &id, &type, name);
    if (ResError() != noErr || name[0] == 0)
        return 0;
    char family[256];
    pstr_to_utf8(name, family, sizeof family);
    short file = HomeResFile(fond);
    if (file == -1)
        return 0;
    long size = GetHandleSize(fond);
    /* FamRec is 52 bytes; the font association table follows: a count
     * minus one, then (size, style, resource ID) triples. */
    if (size < 54)
        return 0;
    SignedByte state = HGetState(fond);
    HLock(fond);
    const unsigned char *p = (const unsigned char *)*fond;
    int count = be16(p + 52) + 1;
    int added = 0;
    for (int i = 0; i < count && 54 + (i + 1) * 6 <= size; i++) {
        const unsigned char *e = p + 54 + i * 6;
        uint16_t pt = be16(e), style = be16(e + 2);
        short sfnt = (short)be16(e + 4);
        if (pt != 0)
            continue; /* a bitmap size */
        FontRef *ref = au_calloc(sizeof *ref);
        ref->file = file;
        ref->id = sfnt;
        int weight = (style & 1) ? 700 : 400;
        bool italic = (style & 2) != 0;
        au_fontset_add_lazy(set, family, weight, italic, 0, 0, load_sfnt, ref);
        added++;
    }
    HSetState(fond, state);
    return added;
}

static int scan_chain(AuFontSet *set)
{
    int added = 0;
    short n = CountResources('FOND');
    for (short i = 1; i <= n; i++) {
        Handle h = GetIndResource('FOND', i);
        if (h)
            added += add_fond(set, h);
    }
    return added;
}

/* The fallback: open each file in the Fonts folder and read its FONDs. */
static int scan_fonts_folder(AuFontSet *set)
{
    short vref;
    long dir;
    /* kOnSystemDisk (-32768), kFontsFolderType ('font') */
    if (FindFolder(-32768, 'font', false, &vref, &dir) != noErr)
        return 0;
    int added = 0;
    for (short index = 1; index < 1000; index++) {
        CInfoPBRec pb;
        Str255 name;
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vref;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = index;
        if (PBGetCatInfoSync(&pb) != noErr)
            break;
        if (pb.hFileInfo.ioFlAttrib & 0x10)
            continue; /* a folder */
        FSSpec spec;
        if (FSMakeFSSpec(vref, dir, name, &spec) != noErr)
            continue;
        short saved = CurResFile();
        short ref = FSpOpenResFile(&spec, fsRdPerm);
        if (ref == -1)
            continue;
        /* The file stays open: its faces load from it later. */
        UseResFile(ref);
        short n = Count1Resources('FOND');
        for (short i = 1; i <= n; i++) {
            Handle h = Get1IndResource('FOND', i);
            if (h)
                added += add_fond(set, h);
        }
        UseResFile(saved);
    }
    return added;
}

static bool have_family(AuFontSet *set, const char *family)
{
    for (size_t i = 0; i < au_fontset_count(set); i++) {
        const char *f = au_fontset_entry(set, i, NULL, NULL);
        if (au_ieq(f, strlen(f), family, strlen(family)))
            return true;
    }
    return false;
}

static void map_generic(AuFontSet *set, AuGenericFamily g, const char *a, const char *b, const char *c)
{
    const char *opts[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        if (opts[i] && have_family(set, opts[i])) {
            au_fontset_set_generic(set, g, opts[i]);
            return;
        }
    }
}

void au_platform_load_fonts(AuFontSet *set)
{
    if (scan_chain(set) == 0)
        scan_fonts_folder(set);
    map_generic(set, AU_GENERIC_SERIF, "Times New Roman", "Times", "Georgia");
    map_generic(set, AU_GENERIC_SANS_SERIF, "Helvetica", "Arial", "Geneva");
    map_generic(set, AU_GENERIC_MONOSPACE, "Courier New", "Courier", "Monaco");
    map_generic(set, AU_GENERIC_SYSTEM_UI, "Charcoal", "Geneva", "Helvetica");
}
