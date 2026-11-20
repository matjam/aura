/* Aura's folder in the Preferences folder, safe saving, file: URLs and
 * the Open and Save dialogs.
 *
 * Which files Aura keeps, how they are parsed and when they are saved is
 * decided in src/browser/app.c; this file gives it the Mac OS 9 folder
 * primitives (AuAppFiles) and the clocks (AuAppPort). The files live in
 * a folder "Aura" inside the system Preferences folder (FindFolder
 * kPreferencesFolderType), creator 'AURA'. Files of the folder itself are
 * type 'TEXT'; files of the Cache folder 'AuCa', of the Local Storage
 * folder 'AuLS'.
 *
 * A file of the folder itself is written as "<name>.tmp" and then swapped
 * with the old one (FSpExchangeFiles; on volumes without it, delete and
 * rename), then the volume is flushed, so a crash mid-write leaves the
 * old file intact. A file of a subfolder (the cache, web storage) is
 * written as "<name>.tmp" and renamed over the old one, without a flush:
 * those writes are many and a lost one costs only a fetch. */
#include "mac.h"
#include "clock_mac.h"

#include <stdio.h>
#include <string.h>

static short g_vref;
static long g_dir;
static bool g_ok;

static void cpstr(const char *s, Str255 out)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    out[0] = (unsigned char)n;
    memcpy(out + 1, s, n);
}

AuTime mac_now(void)
{
    /* UTC, as cookies and certificates see it (clock_mac.c applies the
     * Map control panel's offset to the local system clock). */
    return (AuTime)au_mac_wall_time();
}

bool storage_init(void)
{
    short vref;
    long dir;
    /* kOnSystemDisk, create the Preferences folder if it is missing. */
    OSErr err = FindFolder(-32768, kPreferencesFolderType, true, &vref, &dir);
    if (err != noErr)
        return false;
    FSSpec spec;
    err = FSMakeFSSpec(vref, dir, (ConstStr255Param) "\pAura", &spec);
    long sub = 0;
    if (err == fnfErr) {
        err = FSpDirCreate(&spec, -1 /* smSystemScript */, &sub);
        if (err != noErr)
            return false;
    } else if (err == noErr) {
        CInfoPBRec pb;
        Str255 name;
        memcpy(name, spec.name, spec.name[0] + 1);
        memset(&pb, 0, sizeof pb);
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = spec.vRefNum;
        pb.dirInfo.ioDrDirID = spec.parID;
        pb.dirInfo.ioFDirIndex = 0;
        if (PBGetCatInfoSync(&pb) != noErr || !(pb.dirInfo.ioFlAttrib & 0x10))
            return false; /* "Aura" exists but is not a folder */
        sub = pb.dirInfo.ioDrDirID;
    } else {
        return false;
    }
    g_vref = vref;
    g_dir = sub;
    g_ok = true;
    return true;
}

bool storage_subfolder(const char *fname, short *vref, long *dir)
{
    if (!g_ok)
        return false;
    Str255 name;
    cpstr(fname, name);
    FSSpec spec;
    OSErr err = FSMakeFSSpec(g_vref, g_dir, name, &spec);
    if (err == fnfErr) {
        long sub = 0;
        if (FSpDirCreate(&spec, -1, &sub) != noErr)
            return false;
        *vref = g_vref;
        *dir = sub;
        return true;
    }
    if (err != noErr)
        return false;
    CInfoPBRec pb;
    Str255 n2;
    memcpy(n2, spec.name, spec.name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = n2;
    pb.dirInfo.ioVRefNum = spec.vRefNum;
    pb.dirInfo.ioDrDirID = spec.parID;
    if (PBGetCatInfoSync(&pb) != noErr || !(pb.dirInfo.ioFlAttrib & 0x10))
        return false;
    *vref = g_vref;
    *dir = pb.dirInfo.ioDrDirID;
    return true;
}

/* ---- The folders: the Aura folder itself (folder NULL), and folders
 * inside it, found once and remembered ---- */

typedef struct MacFolder {
    char name[32];
    short vref;
    long dir;
    OSType type;
} MacFolder;

static MacFolder folders[6];
static int nfolders;

typedef struct Loc {
    short vref;
    long dir;
    OSType type;
    bool top; /* the Aura folder itself */
} Loc;

static bool locate(const char *folder, Loc *loc)
{
    if (!g_ok)
        return false;
    if (!folder) {
        loc->vref = g_vref;
        loc->dir = g_dir;
        loc->type = 'TEXT';
        loc->top = true;
        return true;
    }
    int i;
    for (i = 0; i < nfolders; i++)
        if (!strcmp(folders[i].name, folder))
            break;
    if (i == nfolders) {
        short vref;
        long dir;
        if (strlen(folder) >= sizeof folders[0].name || nfolders == (int)(sizeof folders / sizeof folders[0]) ||
            !storage_subfolder(folder, &vref, &dir))
            return false;
        MacFolder *f = &folders[nfolders++];
        snprintf(f->name, sizeof f->name, "%s", folder);
        f->vref = vref;
        f->dir = dir;
        f->type = !strcmp(folder, AU_APP_FOLDER_CACHE)           ? 'AuCa'
                  : !strcmp(folder, AU_APP_FOLDER_LOCAL_STORAGE) ? 'AuLS'
                  : !strcmp(folder, AU_APP_FOLDER_SCRIPT_CACHE)  ? 'AuSC'
                                                                 : 'TEXT';
    }
    loc->vref = folders[i].vref;
    loc->dir = folders[i].dir;
    loc->type = folders[i].type;
    loc->top = false;
    return true;
}

/* A file of a folder; true also when it does not exist (fnfErr). */
static bool spec_in(const Loc *loc, const char *name, FSSpec *spec)
{
    Str255 p;
    if (strlen(name) > 31)
        return false;
    cpstr(name, p);
    OSErr err = FSMakeFSSpec(loc->vref, loc->dir, p, spec);
    return err == noErr || err == fnfErr;
}

static bool f_read(void *ctx, const char *folder, const char *name, AuBuf *out, size_t max)
{
    (void)ctx;
    Loc loc;
    FSSpec spec;
    short ref;
    if (!locate(folder, &loc) || !spec_in(&loc, name, &spec) || FSpOpenDF(&spec, fsRdPerm, &ref) != noErr)
        return false;
    long eof = 0;
    bool ok = false;
    if (GetEOF(ref, &eof) == noErr && eof >= 0 && (unsigned long)eof < max) {
        if (eof == 0) {
            ok = true;
        } else {
            au_buf_reserve(out, (size_t)eof);
            long count = eof;
            OSErr err = FSRead(ref, &count, out->data + out->len);
            if ((err == noErr || err == eofErr) && count == eof) {
                out->len += (size_t)eof;
                ok = true;
            }
        }
    }
    FSClose(ref);
    return ok;
}

/* The Aura folder's files: written to "<name>.tmp", then swapped in. */
static bool write_swapped(const Loc *loc, const char *name, const void *data, size_t len)
{
    char tmpname[40];
    snprintf(tmpname, sizeof tmpname, "%s.tmp", name);
    Str255 pname;
    cpstr(name, pname);
    FSSpec tmp, dst;
    if (!spec_in(loc, tmpname, &tmp))
        return false;
    FSpDelete(&tmp);
    if (FSpCreate(&tmp, 'AURA', loc->type, smSystemScript) != noErr)
        return false;
    short ref;
    if (FSpOpenDF(&tmp, fsRdWrPerm, &ref) != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    long count = (long)len;
    OSErr err = FSWrite(ref, &count, data);
    if (err == noErr && count == (long)len)
        err = SetEOF(ref, (long)len);
    else if (err == noErr)
        err = ioErr;
    OSErr cerr = FSClose(ref);
    if (err != noErr || cerr != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    err = FSMakeFSSpec(loc->vref, loc->dir, pname, &dst);
    if (err == noErr) {
        if (FSpExchangeFiles(&tmp, &dst) == noErr) {
            FSpDelete(&tmp); /* now holds the old contents */
        } else {
            if (FSpDelete(&dst) != noErr) {
                FSpDelete(&tmp);
                return false;
            }
            if (FSpRename(&tmp, pname) != noErr)
                return false;
        }
    } else if (err == fnfErr) {
        if (FSpRename(&tmp, pname) != noErr)
            return false;
    } else {
        FSpDelete(&tmp);
        return false;
    }
    FlushVol(NULL, loc->vref);
    return true;
}

/* A subfolder's files: written to "<name>.tmp", then renamed over the
 * old file. */
static bool write_renamed(const Loc *loc, const char *name, const void *data, size_t len)
{
    char tname[40];
    snprintf(tname, sizeof tname, "%s.tmp", name);
    FSSpec tmp, dst;
    if (!spec_in(loc, tname, &tmp) || !spec_in(loc, name, &dst))
        return false;
    FSpDelete(&tmp);
    if (FSpCreate(&tmp, 'AURA', loc->type, smSystemScript) != noErr)
        return false;
    short ref;
    if (FSpOpenDF(&tmp, fsRdWrPerm, &ref) != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    long count = (long)len;
    OSErr err = FSWrite(ref, &count, data);
    if (err == noErr && count != (long)len)
        err = dskFulErr;
    OSErr cerr = FSClose(ref);
    if (err != noErr || cerr != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    FSpDelete(&dst);
    Str255 p;
    cpstr(name, p);
    if (FSpRename(&tmp, p) != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    return true;
}

static bool f_write(void *ctx, const char *folder, const char *name, const void *data, size_t len)
{
    (void)ctx;
    Loc loc;
    if (!locate(folder, &loc) || strlen(name) > 27)
        return false;
    return loc.top ? write_swapped(&loc, name, data, len) : write_renamed(&loc, name, data, len);
}

static bool f_rename(void *ctx, const char *folder, const char *name, const char *new_name)
{
    (void)ctx;
    Loc loc;
    FSSpec spec, old;
    if (!locate(folder, &loc) || !spec_in(&loc, new_name, &old) || !spec_in(&loc, name, &spec))
        return false;
    FSpDelete(&old);
    Str255 p;
    cpstr(new_name, p);
    return FSpRename(&spec, p) == noErr;
}

static void f_remove(void *ctx, const char *folder, const char *name)
{
    (void)ctx;
    Loc loc;
    FSSpec spec;
    if (!locate(folder, &loc) || !spec_in(&loc, name, &spec))
        return;
    if (FSpDelete(&spec) == noErr && loc.top)
        FlushVol(NULL, loc.vref);
}

static bool f_exists(void *ctx, const char *folder, const char *name)
{
    (void)ctx;
    Loc loc;
    FSSpec spec;
    FInfo fi;
    return locate(folder, &loc) && spec_in(&loc, name, &spec) && FSpGetFInfo(&spec, &fi) == noErr;
}

static bool f_make_folder(void *ctx, const char *folder)
{
    (void)ctx;
    Loc loc;
    return locate(folder, &loc);
}

static void f_list(void *ctx, const char *folder, void (*each)(void *, const char *, size_t, uint32_t), void *ec)
{
    (void)ctx;
    Loc loc;
    if (!locate(folder, &loc))
        return;
    for (short i = 1; i < 32000; i++) {
        CInfoPBRec pb;
        Str255 name;
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = loc.vref;
        pb.hFileInfo.ioDirID = loc.dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr)
            break;
        if (pb.hFileInfo.ioFlAttrib & 0x10)
            continue;
        char cname[32];
        memcpy(cname, name + 1, name[0]);
        cname[name[0]] = 0;
        size_t nl = name[0];
        if (nl > 4 && !strcmp(cname + nl - 4, ".tmp")) {
            /* An interrupted write. */
            FSSpec s;
            if (spec_in(&loc, cname, &s) && FSpDelete(&s) == noErr)
                i--; /* the folder's indices moved down */
            continue;
        }
        each(ec, cname, (size_t)pb.hFileInfo.ioFlLgLen, (uint32_t)pb.hFileInfo.ioFlMdDat);
    }
}

static uint32_t c_ms(void *ctx)
{
    (void)ctx;
    return au_mac_clock_ms();
}

static uint32_t c_us(void *ctx)
{
    (void)ctx;
    return au_mac_clock_us();
}

static int64_t c_wall(void *ctx)
{
    (void)ctx;
    return au_mac_wall_time();
}

static int64_t c_wall_ms(void *ctx)
{
    (void)ctx;
    return au_mac_wall_ms();
}

static int32_t c_offset(void *ctx)
{
    (void)ctx;
    return au_mac_gmt_offset();
}

void storage_port(AuAppPort *p)
{
    memset(p, 0, sizeof *p);
    p->files.read = f_read;
    p->files.write = f_write;
    p->files.rename = f_rename;
    p->files.remove = f_remove;
    p->files.exists = f_exists;
    p->files.make_folder = f_make_folder;
    p->files.list = f_list;
    p->ms = c_ms;
    p->us = c_us;
    p->wall_time = c_wall;
    p->wall_ms = c_wall_ms;
    p->utc_offset = c_offset;
}

bool mac_read_file_url(const char *url, char **data, size_t *len)
{
    char *path = au_url_to_path(url, ':');
    if (!path)
        return false;
    size_t n;
    char *roman = mac_utf8_to_roman(path, &n);
    au_free(path);
    bool ok = n <= 255 && au_platform_read_file(roman, data, len);
    au_free(roman);
    return ok;
}

static OSErr write_spec(FSSpec *spec, const char *data, size_t len)
{
    OSErr err = FSpCreate(spec, 'AURA', 'TEXT', smSystemScript);
    if (err == dupFNErr) {
        /* Replacing: keep the file, give it Aura's type and creator. */
        FInfo fi;
        err = FSpGetFInfo(spec, &fi);
        if (err == noErr) {
            fi.fdType = 'TEXT';
            fi.fdCreator = 'AURA';
            err = FSpSetFInfo(spec, &fi);
        }
    }
    if (err != noErr)
        return err;
    short ref;
    err = FSpOpenDF(spec, fsRdWrPerm, &ref);
    if (err != noErr)
        return err;
    long count = (long)len;
    err = SetEOF(ref, 0);
    if (err == noErr)
        err = FSWrite(ref, &count, data);
    OSErr cerr = FSClose(ref);
    if (err == noErr)
        err = cerr;
    if (err == noErr)
        FlushVol(NULL, spec->vRefNum);
    return err;
}

void mac_save_page(AuBrowserWindow *w)
{
    char name[128];
    au_bw_save_name(w, name, sizeof name);
    Str255 pname;
    mac_pstr(name, pname);
    if (pname[0] > 31)
        pname[0] = 31;
    StandardFileReply reply;
    memset(&reply, 0, sizeof reply);
    StandardPutFile((ConstStringPtr) "\pSave page source as:", pname, &reply);
    if (!reply.sfGood)
        return;
    /* Fetch the bytes after the dialog: nothing loads while it is up,
     * but the page could have changed before. */
    const char *data;
    size_t len;
    OSErr err = au_bw_page_source(w, &data, &len) ? write_spec(&reply.sfFile, data, len) : fnfErr;
    if (err != noErr) {
        Str255 msg;
        char line[96];
        snprintf(line, sizeof line, "The page could not be saved (error %d).", (int)err);
        mac_pstr(line, msg);
        ParamText(msg, (ConstStringPtr) "\p", (ConstStringPtr) "\p", (ConstStringPtr) "\p");
        StopAlert(132, NULL);
    }
}

bool mac_save_text(const char *prompt, const char *name, const char *text, size_t len)
{
    Str255 pprompt, pname;
    mac_pstr(prompt, pprompt);
    mac_pstr(name, pname);
    if (pname[0] > 31)
        pname[0] = 31;
    StandardFileReply reply;
    memset(&reply, 0, sizeof reply);
    StandardPutFile(pprompt, pname, &reply);
    if (!reply.sfGood)
        return false;
    OSErr err = write_spec(&reply.sfFile, text, len);
    if (err != noErr) {
        Str255 msg;
        char line[96];
        snprintf(line, sizeof line, "The file could not be saved (error %d).", (int)err);
        mac_pstr(line, msg);
        ParamText(msg, (ConstStringPtr) "\p", (ConstStringPtr) "\p", (ConstStringPtr) "\p");
        StopAlert(132, NULL);
        return false;
    }
    return true;
}

char *mac_choose_file_url(void)
{
    StandardFileReply reply;
    memset(&reply, 0, sizeof reply);
    StandardGetFile(NULL, -1, NULL, &reply);
    if (!reply.sfGood)
        return NULL;
    /* The full path: the file's name, then each parent up to the volume. */
    char path[1024];
    size_t n = reply.sfFile.name[0];
    memcpy(path, reply.sfFile.name + 1, n);
    path[n] = 0;
    long dir = reply.sfFile.parID;
    for (int depth = 0; depth < 64; depth++) {
        CInfoPBRec pb;
        Str255 name;
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = reply.sfFile.vRefNum;
        pb.dirInfo.ioFDirIndex = -1;
        pb.dirInfo.ioDrDirID = dir;
        if (PBGetCatInfoSync(&pb) != noErr)
            return NULL;
        size_t k = name[0];
        size_t cur = strlen(path);
        if (k + 1 + cur + 1 > sizeof path)
            return NULL;
        memmove(path + k + 1, path, cur + 1);
        memcpy(path, name + 1, k);
        path[k] = ':';
        if (dir == 2) /* fsRtDirID: the volume */
            break;
        dir = pb.dirInfo.ioDrParID;
    }
    char *utf8 = mac_roman_to_utf8(path, strlen(path));
    char *url = au_url_from_path(utf8, ':');
    au_free(utf8);
    return url;
}
