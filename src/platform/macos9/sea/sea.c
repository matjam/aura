/* The Aura installer: a self-extracting archive for Mac OS 9.
 *
 * tools/make-sea.py appends an archive of the distributable folder to
 * this application's data fork, after the PowerPC code. When opened, the
 * application asks where to put the folder, creates it, writes every file
 * with both forks, its type, creator, Finder flags and dates, checks each
 * fork's CRC-32, opens the new folder in the Finder and quits. A failure
 * (disk full, locked disk, damaged archive, Command-period) removes the
 * partial folder and says what happened.
 *
 * The archive (big-endian; tools/make-sea.py writes it):
 *
 *   header   "AuraSEA1", u32 entry count, u32 total bytes of all forks,
 *            Pascal string in 32 bytes: the folder's name
 *   entry    u32 'AuEn', u8 kind (0 folder, 1 file), u8 0, u16 path length,
 *            type, creator, u16 Finder flags, u16 0,
 *            u32 creation date, u32 modification date (Mac OS seconds),
 *            data fork: u32 length, u32 compressed length, u32 CRC-32,
 *            resource fork: the same three,
 *            the path (Mac Roman, components separated by ':', relative
 *            to the folder), u32 CRC-32 of the entry up to here,
 *            the data fork, then the resource fork, each as a raw deflate
 *            stream (nothing for an empty fork)
 *   trailer  the data fork's last 20 bytes: "AuraSEA1", u32 offset of the
 *            header in the fork, u32 length of the archive from the header
 *            to the trailer, u32 CRC-32 of the header
 *
 * Forks are streamed through two fixed buffers and Aura's inflate decoder
 * (src/codec), so the memory needed does not depend on the archive. */
#include <Multiverse.h>

#include "codec/codec.h"

#include <string.h>

enum {
    kAlertStop = 128,   /* ^0 ^1, OK */
    kAlertRename = 129, /* ^0 exists, use ^1? OK, Cancel */

    kErrDamaged = 1, /* the archive is not what the packer wrote */
    kErrStopped = 2, /* Command-period */

    kInSize = 16 * 1024,
    kOutSize = 32 * 1024,
    kHeaderSize = 48,
    kEntrySize = 52,
    kTrailerSize = 20,
    kMaxPath = 240,

    kBarLeft = 16,
    kBarTop = 44,
    kBarRight = 344,
    kBarBottom = 56
};

static const char kMagic[] = "AuraSEA1";

/* A Pascal string literal as the Toolbox takes it. */
#define P(s) ((ConstStringPtr)(s))

typedef struct {
    unsigned char kind;
    OSType type, creator;
    unsigned short flags;
    unsigned long created, modified;
    unsigned long data_len, data_clen, data_crc;
    unsigned long rsrc_len, rsrc_clen, rsrc_crc;
    Str255 path; /* ":Fonts:inter:Inter.ttf", relative to the folder */
} SeaEntry;

static short g_archive;       /* this application's data fork */
static unsigned long g_left;  /* archive bytes not yet read */
static AuInflate *g_inflate;
static unsigned char *g_in, *g_out;

static WindowPtr g_window;
static unsigned long g_total, g_done;
static short g_bar;           /* pixels of the bar filled */
static Str63 g_label;         /* the file being written */
static unsigned long g_polled; /* TickCount of the last event poll */

/* ---- Small helpers ---- */

static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

static void pstr_copy(StringPtr to, ConstStringPtr from, int max)
{
    int n = from[0] > max ? max : from[0];
    to[0] = (unsigned char)n;
    memcpy(to + 1, from + 1, (size_t)n);
}

static void pstr_cat_c(StringPtr to, const char *s)
{
    while (*s && to[0] < 255)
        to[++to[0]] = (unsigned char)*s++;
}

static void pstr_cat(StringPtr to, ConstStringPtr s)
{
    int i;
    for (i = 1; i <= s[0] && to[0] < 255; i++)
        to[++to[0]] = s[i];
}

static void stop_alert(ConstStringPtr what, ConstStringPtr detail)
{
    InitCursor();
    ParamText(what, detail, P("\p"), P("\p"));
    StopAlert(kAlertStop, NULL);
}

/* ---- Reading the archive ---- */

static OSErr sea_read(void *to, unsigned long n)
{
    long count = (long)n;
    OSErr err;
    if (n > g_left)
        return kErrDamaged;
    err = FSRead(g_archive, &count, to);
    if (err == eofErr || (err == noErr && (unsigned long)count != n))
        return kErrDamaged;
    g_left -= n;
    return err;
}

/* Opens this application's data fork and reads the trailer and the
 * header. Returns the folder's name and sets g_total. */
static OSErr sea_open(FSSpec *self, StringPtr folder, unsigned long *entries)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    unsigned char b[kHeaderSize];
    long eof, pos;
    unsigned long offset, length;
    OSErr err;

    GetCurrentProcess(&psn);
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = self;
    err = GetProcessInformation(&psn, &info);
    if (err != noErr)
        return err;
    err = HOpenDF(self->vRefNum, self->parID, self->name, fsRdPerm, &g_archive);
    if (err != noErr)
        return err;
    err = GetEOF(g_archive, &eof);
    if (err != noErr)
        return err;
    if (eof < kTrailerSize + kHeaderSize)
        return kErrDamaged;
    err = SetFPos(g_archive, fsFromStart, eof - kTrailerSize);
    if (err != noErr)
        return err;
    g_left = kTrailerSize;
    err = sea_read(b, kTrailerSize);
    if (err != noErr)
        return err;
    offset = be32(b + 8);
    length = be32(b + 12);
    if (memcmp(b, kMagic, 8) != 0 || length < kHeaderSize || offset > (unsigned long)eof ||
        length != (unsigned long)eof - kTrailerSize - offset)
        return kErrDamaged;
    pos = (long)offset;
    err = SetFPos(g_archive, fsFromStart, pos);
    if (err != noErr)
        return err;
    g_left = length;
    {
        unsigned long want = be32(b + 16);
        err = sea_read(b, kHeaderSize);
        if (err != noErr)
            return err;
        if (memcmp(b, kMagic, 8) != 0 || au_codec_crc32(0, b, kHeaderSize) != want || b[16] == 0 || b[16] > 31)
            return kErrDamaged;
    }
    *entries = be32(b + 8);
    g_total = be32(b + 12);
    pstr_copy(folder, b + 16, 31);
    return noErr;
}

/* Reads the next entry's header. The path gets a leading colon, which
 * makes it a partial path name; a component may not be empty, since "::"
 * names the parent folder. */
static OSErr sea_next(SeaEntry *e)
{
    unsigned char b[kEntrySize + kMaxPath + 4];
    unsigned long n;
    unsigned i;
    OSErr err = sea_read(b, kEntrySize);
    if (err != noErr)
        return err;
    n = (unsigned long)b[6] << 8 | b[7];
    if (be32(b) != 'AuEn' || b[4] > 1 || n == 0 || n > kMaxPath)
        return kErrDamaged;
    err = sea_read(b + kEntrySize, n + 4);
    if (err != noErr)
        return err;
    if (au_codec_crc32(0, b, kEntrySize + n) != be32(b + kEntrySize + n))
        return kErrDamaged;
    e->kind = b[4];
    e->type = be32(b + 8);
    e->creator = be32(b + 12);
    e->flags = (unsigned short)(b[16] << 8 | b[17]);
    e->created = be32(b + 20);
    e->modified = be32(b + 24);
    e->data_len = be32(b + 28);
    e->data_clen = be32(b + 32);
    e->data_crc = be32(b + 36);
    e->rsrc_len = be32(b + 40);
    e->rsrc_clen = be32(b + 44);
    e->rsrc_crc = be32(b + 48);
    e->path[0] = (unsigned char)(n + 1);
    e->path[1] = ':';
    memcpy(e->path + 2, b + kEntrySize, n);
    for (i = 2; i <= e->path[0]; i++) {
        if (e->path[i] == ':' && (e->path[i - 1] == ':' || i == e->path[0]))
            return kErrDamaged;
    }
    return noErr;
}

/* ---- The progress window ---- */

static void draw_bar(void)
{
    Rect r;
    SetRect(&r, kBarLeft, kBarTop, kBarLeft + g_bar, kBarBottom);
    FillRect(&r, &qd.dkGray);
}

static void draw_label(void)
{
    Rect r;
    SetRect(&r, kBarLeft, 12, kBarRight, 34);
    EraseRect(&r);
    MoveTo(kBarLeft, 28);
    DrawString(g_label);
}

static void draw_window(void)
{
    Rect r;
    SetPort(g_window);
    draw_label();
    SetRect(&r, kBarLeft - 1, kBarTop - 1, kBarRight + 1, kBarBottom + 1);
    FrameRect(&r);
    draw_bar();
    MoveTo(kBarLeft, 78);
    DrawString(P("\pTo stop, press Command-period."));
}

static void open_window(void)
{
    Rect r = qd.screenBits.bounds;
    short w = 360, h = 92;
    short left = (short)((r.left + r.right - w) / 2);
    short top = (short)(r.top + (r.bottom - r.top - h) / 3);
    SetRect(&r, left, top, left + w, top + h);
    g_window = NewWindow(NULL, &r, P("\pAura Installer"), true, movableDBoxProc, (WindowPtr)-1, false, 0);
    if (g_window) {
        SetPort(g_window);
        TextFont(systemFont);
        TextSize(12);
        draw_window();
    }
}

/* Handles pending events. Returns kErrStopped on Command-period. */
static OSErr poll_events(void)
{
    EventRecord ev;
    WindowPtr w;
    OSErr result = noErr;
    g_polled = TickCount();
    while (WaitNextEvent(everyEvent, &ev, 0, NULL)) {
        switch (ev.what) {
        case updateEvt:
            w = (WindowPtr)ev.message;
            BeginUpdate(w);
            if (w == g_window)
                draw_window();
            EndUpdate(w);
            break;
        case mouseDown:
            if (FindWindow(ev.where, &w) == inDrag && w == g_window) {
                Rect limit = qd.screenBits.bounds;
                InsetRect(&limit, 4, 4);
                DragWindow(w, ev.where, &limit);
            }
            break;
        case keyDown:
        case autoKey:
            if ((ev.modifiers & cmdKey) && (ev.message & charCodeMask) == '.')
                result = kErrStopped;
            break;
        case kHighLevelEvent:
            AEProcessAppleEvent(&ev);
            break;
        default:
            break;
        }
    }
    if (g_window)
        SetPort(g_window);
    return result;
}

static void set_label(ConstStringPtr path)
{
    int i, start = 1;
    if (!g_window)
        return;
    for (i = 1; i <= path[0]; i++) {
        if (path[i] == ':')
            start = i + 1;
    }
    g_label[0] = 0;
    pstr_cat_c(g_label, "Writing \322");
    for (i = start; i <= path[0] && g_label[0] < 60; i++)
        g_label[++g_label[0]] = path[i];
    pstr_cat_c(g_label, "\323");
    draw_label();
}

static void advance(unsigned long n)
{
    short bar;
    g_done += n;
    if (!g_window || g_total == 0)
        return;
    /* Kilobytes, so that the product stays within 32 bits. */
    bar = (short)((g_done >> 10) * (unsigned long)(kBarRight - kBarLeft) / ((g_total >> 10) + 1));
    if (bar > kBarRight - kBarLeft)
        bar = kBarRight - kBarLeft;
    if (bar != g_bar) {
        g_bar = bar;
        draw_bar();
    }
}

/* ---- Writing files ---- */

/* Inflates one fork from the archive into the open file `out`. */
static OSErr put_fork(short out, unsigned long len, unsigned long clen, unsigned long want_crc)
{
    unsigned long crc = 0, total = 0;
    const uint8_t *in = g_in;
    size_t in_len = 0;
    AuCodecStatus st = AU_CODEC_OK;
    OSErr err;

    if (len == 0 && clen == 0)
        return noErr;
    au_inflate_reset(g_inflate);
    while (st != AU_CODEC_DONE) {
        uint8_t *o = g_out;
        size_t out_len = kOutSize;
        long made;
        if (in_len == 0) {
            unsigned long n = clen < kInSize ? clen : kInSize;
            if (n == 0)
                return kErrDamaged;
            err = sea_read(g_in, n);
            if (err != noErr)
                return err;
            clen -= n;
            in = g_in;
            in_len = n;
        }
        st = au_inflate_run(g_inflate, &in, &in_len, &o, &out_len);
        if (st == AU_CODEC_ERROR)
            return kErrDamaged;
        made = (long)(kOutSize - out_len);
        if (made > 0) {
            long count = made;
            total += (unsigned long)made;
            if (total > len)
                return kErrDamaged;
            crc = au_codec_crc32(crc, g_out, (size_t)made);
            err = FSWrite(out, &count, g_out);
            if (err != noErr)
                return err;
            advance((unsigned long)made);
        }
        if (TickCount() - g_polled >= 6) {
            err = poll_events();
            if (err != noErr)
                return err;
        }
    }
    if (clen != 0 || in_len != 0 || total != len || crc != want_crc)
        return kErrDamaged;
    return noErr;
}

static OSErr put_file(short vol, long dir, const SeaEntry *e)
{
    CInfoPBRec pb;
    Str255 name;
    short ref;
    OSErr err, err2;

    err = HCreate(vol, dir, e->path, e->creator, e->type);
    if (err != noErr)
        return err;
    err = HOpenDF(vol, dir, e->path, fsWrPerm, &ref);
    if (err != noErr)
        return err;
    err = put_fork(ref, e->data_len, e->data_clen, e->data_crc);
    err2 = FSClose(ref);
    if (err == noErr)
        err = err2;
    if (err != noErr)
        return err;
    if (e->rsrc_len != 0) {
        err = HOpenRF(vol, dir, e->path, fsWrPerm, &ref);
        if (err != noErr)
            return err;
        err = put_fork(ref, e->rsrc_len, e->rsrc_clen, e->rsrc_crc);
        err2 = FSClose(ref);
        if (err == noErr)
            err = err2;
        if (err != noErr)
            return err;
    }

    /* The Finder flags and dates. hasBeenInited stays clear, so the Finder
     * reads the application's bundle when it first shows the file. */
    memset(&pb, 0, sizeof pb);
    pstr_copy(name, e->path, 255);
    pb.hFileInfo.ioNamePtr = name;
    pb.hFileInfo.ioVRefNum = vol;
    pb.hFileInfo.ioDirID = dir;
    err = PBGetCatInfoSync(&pb);
    if (err != noErr)
        return err;
    pb.hFileInfo.ioFlFndrInfo.fdFlags = (unsigned short)(e->flags & ~0x0100);
    pb.hFileInfo.ioFlCrDat = e->created;
    pb.hFileInfo.ioFlMdDat = e->modified;
    pstr_copy(name, e->path, 255);
    pb.hFileInfo.ioNamePtr = name;
    pb.hFileInfo.ioDirID = dir;
    return PBSetCatInfoSync(&pb);
}

/* Deletes folder `root` of volume `vol` and everything in it, without
 * recursion: delete the first item of the current folder until it is
 * empty, then the folder itself, and continue in its parent. */
static void remove_tree(short vol, long root)
{
    long dir = root;
    for (;;) {
        CInfoPBRec pb;
        Str63 name;
        long parent;
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vol;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = 1;
        if (PBGetCatInfoSync(&pb) == noErr) {
            if (pb.hFileInfo.ioFlAttrib & 0x10) {
                dir = pb.dirInfo.ioDrDirID;
                continue;
            }
            if (HDelete(vol, dir, name) != noErr)
                return;
            continue;
        }
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = vol;
        pb.dirInfo.ioDrDirID = dir;
        pb.dirInfo.ioFDirIndex = -1;
        if (PBGetCatInfoSync(&pb) != noErr)
            return;
        parent = pb.dirInfo.ioDrParID;
        if (HDelete(vol, parent, name) != noErr)
            return;
        if (dir == root)
            return;
        dir = parent;
    }
}

static OSErr extract(short vol, long dir, unsigned long entries, StringPtr failed)
{
    SeaEntry e;
    unsigned long i;
    OSErr err = noErr;
    for (i = 0; i < entries && err == noErr; i++) {
        err = sea_next(&e);
        if (err != noErr)
            break;
        pstr_copy(failed, e.path, 255);
        if (e.kind == 0) {
            FSSpec spec;
            long made;
            err = FSMakeFSSpec(vol, dir, e.path, &spec);
            if (err == fnfErr)
                err = FSpDirCreate(&spec, smSystemScript, &made);
            else if (err == noErr)
                err = dupFNErr;
        } else {
            set_label(e.path);
            err = put_file(vol, dir, &e);
        }
        if (err == noErr)
            err = poll_events();
    }
    if (err == noErr && g_left != 0)
        err = kErrDamaged;
    return err;
}

/* ---- Choosing the folder ---- */

static Boolean exists(short vol, long dir, ConstStringPtr name)
{
    FSSpec spec;
    return FSMakeFSSpec(vol, dir, name, &spec) == noErr;
}

/* The first of "Aura", "Aura 2", "Aura 3" ... that folder `dir` lacks. */
static void free_name(short vol, long dir, ConstStringPtr base, StringPtr name)
{
    long n;
    pstr_copy(name, base, 31);
    for (n = 2; n < 1000 && exists(vol, dir, name); n++) {
        Str31 digits;
        NumToString(n, digits);
        pstr_copy(name, base, 31 - 1 - digits[0]);
        pstr_cat_c(name, " ");
        pstr_cat(name, digits);
    }
}

/* Free bytes on a volume, at most 2 GB. */
static unsigned long free_bytes(short vol)
{
    HParamBlockRec pb;
    unsigned long blocks, size;
    memset(&pb, 0, sizeof pb);
    pb.volumeParam.ioVRefNum = vol;
    if (PBHGetVInfoSync(&pb) != noErr)
        return 0x7FFFFFFFUL;
    blocks = (unsigned short)pb.volumeParam.ioVFrBlk;
    size = (unsigned long)pb.volumeParam.ioVAlBlkSiz;
    if (size != 0 && blocks > 0x7FFFFFFFUL / size)
        return 0x7FFFFFFFUL;
    return blocks * size;
}

/* Writes a low-memory global. Not inlined: the compiler warns about a
 * store through a constant address. */
static __attribute__((noinline)) void lowmem_put(unsigned long address, int size, long value)
{
    volatile unsigned char *p = (volatile unsigned char *)address;
    int i;
    for (i = 0; i < size; i++)
        p[i] = (unsigned char)(value >> (8 * (size - 1 - i)));
}

/* Asks where the folder goes. Returns false when the user cancels. */
static Boolean choose_folder(const FSSpec *self, ConstStringPtr folder, FSSpec *target)
{
    for (;;) {
        StandardFileReply reply;
        Str63 name;
        Str255 text;

        /* Start beside the archive. */
        lowmem_put(0x0214, 2, -self->vRefNum); /* SFSaveDisk */
        lowmem_put(0x0398, 4, self->parID);    /* CurDirStore */
        free_name(self->vRefNum, self->parID, folder, name);
        InitCursor();
        StandardPutFile(P("\pPut the Aura folder here, named:"), name, &reply);
        if (!reply.sfGood)
            return false;
        *target = reply.sfFile;
        if (exists(target->vRefNum, target->parID, target->name)) {
            /* Nothing of the user's is replaced: offer the next free name. */
            Str63 other;
            free_name(target->vRefNum, target->parID, target->name, other);
            ParamText(target->name, other, P("\p"), P("\p"));
            if (CautionAlert(kAlertRename, NULL) != 1)
                continue;
            pstr_copy(target->name, other, 31);
        }
        /* Room for the files, with a tenth more for allocation blocks. */
        if (free_bytes(target->vRefNum) < g_total + g_total / 10) {
            Str31 digits;
            NumToString((long)((g_total + g_total / 10) >> 10), digits);
            text[0] = 0;
            pstr_cat_c(text, "Aura needs ");
            pstr_cat(text, digits);
            pstr_cat_c(text, " K of free space. Choose another disk.");
            stop_alert(P("\pThere is not enough room on that disk."), text);
            continue;
        }
        return true;
    }
}

/* ---- After extraction ---- */

/* Asks the Finder to open the folder (an Open Documents event with an
 * alias to it). */
static void open_in_finder(FSSpec *folder)
{
    AliasHandle alias = NULL;
    AEAddressDesc finder;
    AppleEvent ev, reply;
    OSType sig = 'MACS';

    if (NewAlias(NULL, folder, &alias) != noErr || !alias)
        return;
    if (AECreateDesc(typeApplSignature, (Ptr)&sig, sizeof sig, &finder) == noErr) {
        if (AECreateAppleEvent(kCoreEventClass, kAEOpenDocuments, &finder, kAutoGenerateReturnID, kAnyTransactionID,
                               &ev) == noErr) {
            HLock((Handle)alias);
            if (AEPutParamPtr(&ev, keyDirectObject, typeAlias, (Ptr)*alias, GetHandleSize((Handle)alias)) == noErr)
                AESend(&ev, &reply, kAENoReply | kAECanSwitchLayer, kAENormalPriority, kAEDefaultTimeout, NULL, NULL);
            HUnlock((Handle)alias);
            AEDisposeDesc(&ev);
        }
        AEDisposeDesc(&finder);
    }
    DisposeHandle((Handle)alias);
}

static void report(OSErr err, ConstStringPtr path)
{
    Str255 detail;
    Str31 digits;
    ConstStringPtr what;
    switch (err) {
    case kErrStopped:
        return;
    case kErrDamaged:
        what = P("\pThis copy of the Aura installer is damaged.");
        break;
    case dskFulErr:
        what = P("\pThe disk is full.");
        break;
    case wPrErr:
    case vLckdErr:
        what = P("\pThe disk is locked.");
        break;
    case fLckdErr:
    case permErr:
    case wrPermErr:
    case -5000: /* afpAccessDenied */
        what = P("\pYou do not have permission to write there.");
        break;
    case ioErr:
        what = P("\pThe disk reported an error.");
        break;
    case memFullErr:
        what = P("\pThere is not enough memory.");
        break;
    case dupFNErr:
        what = P("\pAn item with that name is already there.");
        break;
    default:
        what = P("\pThe folder could not be written.");
        break;
    }
    detail[0] = 0;
    if (err == kErrDamaged) {
        pstr_cat_c(detail, "It may have been cut short or changed in transfer. Download it again.");
    } else {
        pstr_cat_c(detail, "Nothing was installed.");
    }
    if (path[0]) {
        pstr_cat_c(detail, " (\322");
        pstr_cat(detail, path);
        pstr_cat_c(detail, "\323");
        if (err < 0) {
            NumToString(err, digits);
            pstr_cat_c(detail, ", error ");
            pstr_cat(detail, digits);
        }
        pstr_cat_c(detail, ")");
    }
    stop_alert(what, detail);
}

int main(void)
{
    FSSpec self, target;
    Str63 folder;
    Str255 failed;
    unsigned long entries = 0;
    long dir = 0;
    OSErr err;

    MaxApplZone();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    failed[0] = 0;
    g_in = (unsigned char *)NewPtr(kInSize);
    g_out = (unsigned char *)NewPtr(kOutSize);
    if (!g_in || !g_out) {
        report(memFullErr, failed);
        return 1;
    }
    g_inflate = au_inflate_new(AU_INFLATE_RAW);

    err = sea_open(&self, folder, &entries);
    if (err != noErr) {
        report(err == eofErr ? kErrDamaged : err, failed);
        return 1;
    }
    if (!choose_folder(&self, folder, &target))
        return 0;

    err = FSpDirCreate(&target, smSystemScript, &dir);
    if (err != noErr) {
        report(err, target.name);
        return 1;
    }
    open_window();
    SetCursor(*GetCursor(watchCursor));
    err = extract(target.vRefNum, dir, entries, failed);
    if (err != noErr) {
        remove_tree(target.vRefNum, dir);
        FlushVol(NULL, target.vRefNum);
        if (g_window)
            HideWindow(g_window);
        report(err, failed);
        return 1;
    }
    FlushVol(NULL, target.vRefNum);
    InitCursor();
    open_in_finder(&target);
    return 0;
}
