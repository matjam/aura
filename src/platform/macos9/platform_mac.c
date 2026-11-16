/* The Mac OS 9 platform: time, files and fonts. The window functions are
 * in main.c, which owns the window.
 *
 * Fonts: the system's come first, then the faces bundled in the Fonts
 * folder beside the application (see load_bundled_fonts). The fonts in
 * the System Folder's Fonts folder
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

/* ---- Bundled fonts: the Fonts folder beside the application ----
 * The folder holds .ttf files (data fork) in a subfolder per family and
 * an index, fonts.idx, naming each face's file, weight, style and family.
 * Faces are registered from the index alone; a face's file is read the
 * first time it is matched, into temporary memory when there is room. A
 * file over AU_FONT_PAGED_MIN (the CJK faces) is paged instead: its data
 * fork stays open and the font set reads its tables once and its glyph
 * outlines in 16 KB blocks as they are drawn (fontpage.c). */

typedef struct BundledRef {
    short vref;
    long dir;           /* the Fonts folder */
    unsigned char path[64]; /* partial HFS path from it (Pascal string) */
    Handle mem;
    Ptr ptr;
    bool tried;
    short fork;         /* the open data fork of a paged face, or 0 */
    bool fork_failed;
} BundledRef;

static uint32_t fonts_ms, bundled_ms;
static int bundled_faces;

static bool read_fork(short ref, Handle *hout, Ptr *pout, long *len)
{
    long eof = 0;
    if (GetEOF(ref, &eof) != noErr || eof <= 0)
        return false;
    OSErr err;
    Handle th = TempNewHandle(eof, &err);
    Ptr buf;
    if (th && err == noErr) {
        HLock(th);
        buf = *th;
    } else {
        th = NULL;
        buf = NewPtr(eof);
        if (!buf)
            return false;
    }
    long count = eof;
    if (FSRead(ref, &count, buf) != noErr || count != eof) {
        if (th)
            DisposeHandle(th);
        else
            DisposePtr(buf);
        return false;
    }
    *hout = th;
    *pout = th ? NULL : buf;
    *len = eof;
    return true;
}

static bool load_bundled(void *ctx, const void **data, size_t *len)
{
    BundledRef *b = ctx;
    if (!b->tried) {
        b->tried = true;
        FSSpec spec;
        short ref;
        long n;
        if (FSMakeFSSpec(b->vref, b->dir, b->path, &spec) == noErr && FSpOpenDF(&spec, fsRdPerm, &ref) == noErr) {
            if (!read_fork(ref, &b->mem, &b->ptr, &n))
                b->mem = NULL, b->ptr = NULL;
            FSClose(ref);
        }
    }
    if (!b->mem && !b->ptr)
        return false;
    *data = b->mem ? (const void *)*b->mem : (const void *)b->ptr;
    *len = (size_t)(b->mem ? GetHandleSize(b->mem) : GetPtrSize(b->ptr));
    return true;
}

/* The data fork's length from the catalog, without opening the file. */
static bool bundled_size(void *ctx, uint32_t *len)
{
    BundledRef *b = ctx;
    FSSpec spec;
    if (FSMakeFSSpec(b->vref, b->dir, b->path, &spec) != noErr)
        return false;
    CInfoPBRec pb;
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = spec.name;
    pb.hFileInfo.ioVRefNum = spec.vRefNum;
    pb.hFileInfo.ioDirID = spec.parID;
    pb.hFileInfo.ioFDirIndex = 0;
    if (PBGetCatInfoSync(&pb) != noErr || (pb.hFileInfo.ioFlAttrib & 0x10) || pb.hFileInfo.ioFlLgLen < 0)
        return false;
    *len = (uint32_t)pb.hFileInfo.ioFlLgLen;
    return true;
}

/* Reads n bytes at off of a paged face's file; the fork stays open for
 * the life of the application (the Process Manager closes it at exit). */
static bool bundled_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    BundledRef *b = ctx;
    if (!b->fork) {
        FSSpec spec;
        short ref;
        if (b->fork_failed)
            return false;
        if (FSMakeFSSpec(b->vref, b->dir, b->path, &spec) != noErr || FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) {
            b->fork_failed = true;
            return false;
        }
        b->fork = ref;
    }
    if (n > 0x7FFFFFFFu || off > 0x7FFFFFFFu)
        return false;
    long count = (long)n;
    if (SetFPos(b->fork, fsFromStart, (long)off) != noErr)
        return false;
    return FSRead(b->fork, &count, buf) == noErr && count == (long)n;
}

static const AuFontFileOps bundled_ops = {load_bundled, bundled_size, bundled_read};

typedef struct IndexCtx {
    AuFontSet *set;
    short vref;
    long dir;
} IndexCtx;

static void add_bundled(void *ctx, const AuFontIndexEntry *e)
{
    IndexCtx *ic = ctx;
    size_t n = strlen(e->path);
    if (n + 1 > 62)
        return;
    BundledRef *b = au_calloc(sizeof *b);
    b->vref = ic->vref;
    b->dir = ic->dir;
    /* "a/b.ttf" becomes ":a:b.ttf", relative to the Fonts folder. */
    b->path[0] = (unsigned char)(n + 1);
    b->path[1] = ':';
    for (size_t i = 0; i < n; i++)
        b->path[2 + i] = e->path[i] == '/' ? ':' : (unsigned char)e->path[i];
    au_fontset_add_file(ic->set, e->family, e->weight, e->italic, e->face_index, AU_FONT_BUNDLED, &bundled_ops, b);
    bundled_faces++;
}

/* The folder named name inside (vref, dir), or false. */
static bool sub_folder(short vref, long dir, const unsigned char *name, long *out)
{
    CInfoPBRec pb;
    Str255 nm;
    memcpy(nm, name, (size_t)name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = nm;
    pb.dirInfo.ioVRefNum = vref;
    pb.dirInfo.ioDrDirID = dir;
    pb.dirInfo.ioFDirIndex = 0;
    if (PBGetCatInfoSync(&pb) != noErr || !(pb.dirInfo.ioFlAttrib & 0x10))
        return false;
    *out = pb.dirInfo.ioDrDirID;
    return true;
}

static bool has_ttf_suffix(const unsigned char *p)
{
    if (p[0] < 5)
        return false;
    const unsigned char *s = p + 1 + p[0] - 4;
    return s[0] == '.' && (s[1] | 32) == 't' && (s[2] | 32) == 't' && ((s[3] | 32) == 'f' || (s[3] | 32) == 'c');
}

/* Without an index: describe the fonts in the folder and its subfolders
 * by reading each once (slow; the index exists so this does not run). */
static void describe_folder(AuBuf *b, short vref, long dir, const char *prefix, int depth)
{
    for (short index = 1; index < 500; index++) {
        CInfoPBRec pb;
        Str255 name;
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vref;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = index;
        if (PBGetCatInfoSync(&pb) != noErr)
            break;
        char rel[128];
        size_t pl = strlen(prefix);
        if (pl + name[0] + 2 > sizeof rel)
            continue;
        memcpy(rel, prefix, pl);
        memcpy(rel + pl, name + 1, name[0]);
        rel[pl + name[0]] = 0;
        if (pb.hFileInfo.ioFlAttrib & 0x10) {
            if (depth < 1) {
                long sub = pb.dirInfo.ioDrDirID;
                rel[pl + name[0]] = '/';
                rel[pl + name[0] + 1] = 0;
                describe_folder(b, vref, sub, rel, depth + 1);
            }
            continue;
        }
        if (!has_ttf_suffix(name))
            continue;
        short ref;
        if (HOpenDF(vref, dir, name, fsRdPerm, &ref) != noErr)
            continue;
        Handle h = NULL;
        Ptr p = NULL;
        long len;
        if (read_fork(ref, &h, &p, &len)) {
            au_font_index_write(b, rel, h ? (const void *)*h : (const void *)p, (size_t)len);
            if (h)
                DisposeHandle(h);
            else
                DisposePtr(p);
        }
        FSClose(ref);
    }
}

static void load_bundled_fonts(AuFontSet *set)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec app;
    if (GetCurrentProcess(&psn) != noErr)
        return;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &app;
    if (GetProcessInformation(&psn, &info) != noErr)
        return;
    IndexCtx ic = {set, app.vRefNum, 0};
    if (!sub_folder(app.vRefNum, app.parID, (const unsigned char *)"\005Fonts", &ic.dir))
        return;
    short ref;
    Handle h = NULL;
    Ptr p = NULL;
    long len = 0;
    if (HOpenDF(ic.vref, ic.dir, (const unsigned char *)"\011fonts.idx", fsRdPerm, &ref) == noErr) {
        bool ok = read_fork(ref, &h, &p, &len);
        FSClose(ref);
        if (ok) {
            au_font_index_parse(h ? (const char *)*h : (const char *)p, (size_t)len, add_bundled, &ic);
            if (h)
                DisposeHandle(h);
            else
                DisposePtr(p);
            return;
        }
    }
    AuBuf b;
    au_buf_init(&b);
    describe_folder(&b, ic.vref, ic.dir, "", 0);
    au_font_index_parse(b.data ? b.data : "", b.len, add_bundled, &ic);
    au_buf_free(&b);
}

uint32_t mac_fonts_ms(void)
{
    return fonts_ms;
}

uint32_t mac_fonts_bundled_ms(void)
{
    return bundled_ms;
}

int mac_fonts_bundled(void)
{
    return bundled_faces;
}

void au_platform_load_fonts(AuFontSet *set)
{
    uint32_t t0 = au_platform_time_ms();
    if (scan_chain(set) == 0)
        scan_fonts_folder(set);
    /* After the system's: a system face of the same family wins. */
    uint32_t t1 = au_platform_time_ms();
    load_bundled_fonts(set);
    bundled_ms = au_platform_time_ms() - t1;
    fonts_ms = au_platform_time_ms() - t0;
    map_generic(set, AU_GENERIC_SERIF, "Times New Roman", "Times", "Georgia");
    map_generic(set, AU_GENERIC_SANS_SERIF, "Helvetica", "Arial", "Geneva");
    map_generic(set, AU_GENERIC_MONOSPACE, "Courier New", "Courier", "Monaco");
    map_generic(set, AU_GENERIC_SYSTEM_UI, "Charcoal", "Geneva", "Helvetica");
}
