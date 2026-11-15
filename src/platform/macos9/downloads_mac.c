/* Download files on Mac OS 9: AuDownloadFiles for src/net/transfer.c,
 * the download folder, Reveal in Finder and the application check.
 *
 * A download is written to a partial file in the download folder, named
 * from its id and file name ("12 name.part", cut to 31 characters), so a
 * paused or interrupted download finds its bytes again after a restart.
 * When it completes, the partial file is renamed to the final name, made
 * unique in the folder, and given the type and creator Internet Config
 * maps the name to. Nothing is ever opened automatically. */
#include "mac.h"

#include "net/transfer.h"

#include <stdio.h>
#include <string.h>

typedef struct DlFile {
    short ref;
    short vref;
} DlFile;

static bool folder_dir(const FSSpec *spec, short *vref, long *dir)
{
    CInfoPBRec pb;
    Str255 name;
    memcpy(name, spec->name, spec->name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = name;
    pb.dirInfo.ioVRefNum = spec->vRefNum;
    pb.dirInfo.ioDrDirID = spec->parID;
    if (PBGetCatInfoSync(&pb) != noErr || !(pb.dirInfo.ioFlAttrib & 0x10))
        return false;
    *vref = spec->vRefNum;
    *dir = pb.dirInfo.ioDrDirID;
    return true;
}

bool mac_download_folder(short *vref, long *dir)
{
    const char *pref = g_browser->prefs.download_folder;
    if (pref && *pref) {
        Str255 p;
        mac_pstr(pref, p);
        FSSpec spec;
        if (FSMakeFSSpec(0, 0, p, &spec) == noErr && folder_dir(&spec, vref, dir))
            return true;
    }
    FSSpec ic;
    if (mac_ic_download_folder(&ic) && folder_dir(&ic, vref, dir))
        return true;
    return FindFolder(-32768, kDesktopFolderType, true, vref, dir) == noErr;
}

/* "12 name.part", at most 31 bytes in Mac Roman. */
static void partial_name(const AuDownload *d, Str255 out)
{
    Str255 base;
    mac_pstr(d->filename, base);
    char idbuf[16];
    int il = snprintf(idbuf, sizeof idbuf, "%lu ", (unsigned long)d->id);
    int room = 31 - il - 5;
    int bl = base[0] < room ? base[0] : room;
    out[0] = (unsigned char)(il + bl + 5);
    memcpy(out + 1, idbuf, (size_t)il);
    memcpy(out + 1 + il, base + 1, (size_t)bl);
    memcpy(out + 1 + il + bl, ".part", 5);
}

static char *err_text(const char *what, OSErr err)
{
    char buf[128];
    if (err == dskFulErr)
        snprintf(buf, sizeof buf, "The disk is full");
    else if (err == wPrErr || err == vLckdErr)
        snprintf(buf, sizeof buf, "The download folder is on a locked disk");
    else if (err == fLckdErr || err == permErr)
        snprintf(buf, sizeof buf, "The file is locked");
    else
        snprintf(buf, sizeof buf, "%s (error %d)", what, (int)err);
    return au_strndup(buf, strlen(buf));
}

static void *f_open(void *ctx, AuDownload *d, bool restart, int64_t *existing, char **error)
{
    (void)ctx;
    short vref;
    long dir;
    if (!mac_download_folder(&vref, &dir)) {
        *error = au_strndup("There is no download folder", 27);
        return NULL;
    }
    Str255 name;
    partial_name(d, name);
    FSSpec spec;
    OSErr err = FSMakeFSSpec(vref, dir, name, &spec);
    if (err == fnfErr)
        err = FSpCreate(&spec, 'AURA', 'BINA', smSystemScript);
    if (err != noErr) {
        *error = err_text("Cannot create the file", err);
        return NULL;
    }
    short ref;
    err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
    if (err != noErr) {
        *error = err_text("Cannot open the file", err);
        return NULL;
    }
    long eof = 0;
    if (restart)
        err = SetEOF(ref, 0);
    if (err == noErr)
        err = GetEOF(ref, &eof);
    if (err == noErr)
        err = SetFPos(ref, fsFromStart, eof);
    if (err != noErr) {
        FSClose(ref);
        *error = err_text("Cannot open the file", err);
        return NULL;
    }
    DlFile *f = au_calloc(sizeof *f);
    f->ref = ref;
    f->vref = vref;
    *existing = eof;
    return f;
}

static bool f_write(void *ctx, void *file, const void *bytes, size_t len, char **error)
{
    (void)ctx;
    DlFile *f = file;
    long count = (long)len;
    OSErr err = FSWrite(f->ref, &count, bytes);
    if (err == noErr && count != (long)len)
        err = dskFulErr;
    if (err != noErr) {
        *error = err_text("Cannot write the file", err);
        return false;
    }
    return true;
}

static bool f_has_space(void *ctx, void *file, int64_t bytes)
{
    (void)ctx;
    DlFile *f = file;
    HParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.volumeParam.ioVRefNum = f->vref;
    if (PBHGetVInfoSync(&pb) != noErr)
        return true; /* unknown: let the writes find out */
    int64_t free_bytes = (int64_t)pb.volumeParam.ioVFrBlk * (int64_t)pb.volumeParam.ioVAlBlkSiz;
    return free_bytes >= bytes + 64 * 1024;
}

typedef struct NameCheck {
    short vref;
    long dir;
} NameCheck;

static bool name_taken(void *ctx, const char *name)
{
    NameCheck *nc = ctx;
    Str255 p;
    mac_pstr(name, p);
    FSSpec s;
    return FSMakeFSSpec(nc->vref, nc->dir, p, &s) == noErr;
}

static void type_for(const char *name, OSType *type, OSType *creator)
{
    MacFileMap m;
    if (mac_ic_map_filename(name, &m) && m.type) {
        *type = m.type;
        *creator = m.creator;
        return;
    }
    const char *dot = strrchr(name, '.');
    if (dot && (!strcmp(dot, ".txt") || !strcmp(dot, ".html") || !strcmp(dot, ".htm") || !strcmp(dot, ".css"))) {
        *type = 'TEXT';
        *creator = 'ttxt';
        return;
    }
    *type = 'BINA';
    *creator = '????';
}

static bool f_close(void *ctx, void *file, AuDownload *d, bool complete, char **error)
{
    (void)ctx;
    DlFile *f = file;
    OSErr err = FSClose(f->ref);
    au_free(f);
    if (!complete)
        return true;
    if (err != noErr) {
        if (error)
            *error = err_text("Cannot finish the file", err);
        return false;
    }
    NameCheck nc;
    if (!mac_download_folder(&nc.vref, &nc.dir)) {
        if (error)
            *error = au_strndup("There is no download folder", 27);
        return false;
    }
    Str255 pname;
    partial_name(d, pname);
    FSSpec part;
    if (FSMakeFSSpec(nc.vref, nc.dir, pname, &part) != noErr) {
        if (error)
            *error = au_strndup("The partial file is gone", 24);
        return false;
    }
    char unique[128];
    au_hfs_sanitize(d->filename, name_taken, &nc, unique, sizeof unique);
    Str255 fname;
    mac_pstr(unique, fname);
    err = FSpRename(&part, fname);
    if (err != noErr) {
        if (error)
            *error = err_text("Cannot rename the file", err);
        return false;
    }
    if (strcmp(unique, d->filename)) {
        au_free(d->filename);
        d->filename = au_strndup(unique, strlen(unique));
    }
    FSSpec done;
    if (FSMakeFSSpec(nc.vref, nc.dir, fname, &done) == noErr) {
        FInfo fi;
        if (FSpGetFInfo(&done, &fi) == noErr) {
            type_for(unique, &fi.fdType, &fi.fdCreator);
            FSpSetFInfo(&done, &fi);
        }
    }
    FlushVol(NULL, nc.vref);
    return true;
}

static void f_remove(void *ctx, AuDownload *d)
{
    (void)ctx;
    short vref;
    long dir;
    if (!d->filename || !*d->filename || !mac_download_folder(&vref, &dir))
        return;
    Str255 name;
    partial_name(d, name);
    FSSpec spec;
    if (FSMakeFSSpec(vref, dir, name, &spec) == noErr)
        FSpDelete(&spec);
}

void mac_download_files(AuDownloadFiles *out)
{
    out->ctx = NULL;
    out->open = f_open;
    out->write = f_write;
    out->has_space = f_has_space;
    out->close = f_close;
    out->remove = f_remove;
}

/* Reveal: the Finder's "make objects visible" event ('misc' 'mvis') with
 * the file, then the Finder comes to the front. */
bool mac_download_reveal(const AuDownload *d)
{
    short vref;
    long dir;
    if (d->state != AU_DL_DONE || !mac_download_folder(&vref, &dir))
        return false;
    Str255 name;
    mac_pstr(d->filename, name);
    FSSpec spec;
    if (FSMakeFSSpec(vref, dir, name, &spec) != noErr)
        return false;
    OSType finder = 'MACS';
    AEAddressDesc target;
    if (AECreateDesc(typeApplSignature, &finder, sizeof finder, &target) != noErr)
        return false;
    AppleEvent ev, reply;
    OSErr err = AECreateAppleEvent('misc', 'mvis', &target, kAutoGenerateReturnID, kAnyTransactionID, &ev);
    AEDisposeDesc(&target);
    if (err != noErr)
        return false;
    err = AEPutParamPtr(&ev, keyDirectObject, typeFSS, &spec, sizeof spec);
    if (err == noErr)
        err = AESend(&ev, &reply, kAENoReply, kAENormalPriority, kAEDefaultTimeout, NULL, NULL);
    AEDisposeDesc(&ev);
    if (err != noErr)
        return false;
    ProcessSerialNumber psn = {0, 0};
    ProcessInfoRec info;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        if (GetProcessInformation(&psn, &info) == noErr && info.processSignature == 'MACS') {
            SetFrontProcess(&psn);
            break;
        }
    }
    return true;
}

bool mac_download_is_app(const char *name, const char *mime)
{
    static const char *const exts[] = {".app", ".exe", ".com", ".bat", ".msi", ".smi", ".dmg", ".pkg", ".jar"};
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot) {
        for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
            const char *a = dot, *b = exts[i];
            while (*a && *b && (*a | 32) == *b)
                a++, b++;
            if (!*a && !*b)
                return true;
        }
        MacFileMap m;
        if (mac_ic_map_filename(name, &m) && m.type == 'APPL')
            return true;
    }
    return mime && (!strcmp(mime, "application/x-msdownload") || !strcmp(mime, "application/x-ms-installer") ||
                    !strcmp(mime, "application/vnd.microsoft.portable-executable"));
}
