/* Aura's files in the Preferences folder, safe saving, file: URLs and the
 * Open dialog.
 *
 * Files live in a folder "Aura" inside the system Preferences folder
 * (FindFolder kPreferencesFolderType), type 'TEXT', creator 'AURA':
 *     Aura Preferences   key=value preferences (src/browser/prefs.h)
 *     Bookmarks.html     Netscape bookmark file (bookmarks.h)
 *     History            history log (history.h)
 *     Session            open windows and tabs (browser.h)
 *     Downloads          the downloads list (downloads.h)
 *     Running            exists while Aura runs (unclean exit detection)
 * and, written by network.c through storage_write_named:
 *     Cookies            persistent cookies (cookies.h)
 *     HSTS               Strict-Transport-Security hosts (hsts.h)
 *     Random Seed        the entropy pool's seed (entropy.c writes it)
 *     Cache              a folder of disk cache entries (disk_cache.h)
 *
 * A file is written as "<name>.tmp" and then swapped with the old one
 * (FSpExchangeFiles; on volumes without it, delete and rename), so a crash
 * mid-write leaves the old file intact. A file that fails to parse is
 * renamed "<name> (damaged)" before anything overwrites it.
 *
 * Saving: storage_save_changed compares each data set's change counter
 * with the one last saved and writes it a few seconds after the first
 * unsaved change, or at once when forced (quitting). */
#include "mac.h"
#include "clock_mac.h"

#include <stdio.h>
#include <string.h>

enum { kSaveDelayMs = 3000, kScrollSaveDelayMs = 30000 };

static uint32_t saved_session_gen; /* session_gen when the Session file was last written */

static short g_vref;
static long g_dir;
static bool g_ok;

typedef enum { F_PREFS, F_BOOKMARKS, F_HISTORY, F_SESSION, F_DOWNLOADS, F_COUNT } FileId;

static const char *const file_names[F_COUNT] = {"Aura Preferences", "Bookmarks.html", "History", "Session",
                                                "Downloads"};

typedef struct SaveState {
    uint32_t saved_gen;
    uint32_t seen_gen;
    uint32_t first_change_ms;
    bool pending;
} SaveState;

static SaveState save_state[F_COUNT];

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

bool storage_read_named(const char *fname, char **data, size_t *len)
{
    if (!g_ok)
        return false;
    Str255 name;
    cpstr(fname, name);
    FSSpec spec;
    if (FSMakeFSSpec(g_vref, g_dir, name, &spec) != noErr)
        return false;
    short ref;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr)
        return false;
    long eof = 0;
    bool ok = false;
    if (GetEOF(ref, &eof) == noErr && eof >= 0 && eof < 16L * 1024 * 1024) {
        char *buf = au_alloc((size_t)eof + 1);
        long count = eof;
        OSErr err = FSRead(ref, &count, buf);
        if ((err == noErr || err == eofErr) && count == eof) {
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

void storage_keep_aside_named(const char *fname)
{
    char aside[64];
    snprintf(aside, sizeof aside, "%s (damaged)", fname);
    Str255 name, an;
    cpstr(fname, name);
    cpstr(aside, an);
    FSSpec spec, old;
    if (FSMakeFSSpec(g_vref, g_dir, an, &old) == noErr)
        FSpDelete(&old);
    if (FSMakeFSSpec(g_vref, g_dir, name, &spec) == noErr)
        FSpRename(&spec, an);
}

bool storage_write_named(const char *fname, const char *data, size_t len)
{
    if (!g_ok)
        return false;
    char tmpname[64];
    snprintf(tmpname, sizeof tmpname, "%s.tmp", fname);
    Str255 name, tname;
    cpstr(fname, name);
    cpstr(tmpname, tname);
    FSSpec tmp, dst;
    OSErr err = FSMakeFSSpec(g_vref, g_dir, tname, &tmp);
    if (err == noErr)
        FSpDelete(&tmp);
    else if (err != fnfErr)
        return false;
    if (FSpCreate(&tmp, 'AURA', 'TEXT', -1) != noErr)
        return false;
    short ref;
    if (FSpOpenDF(&tmp, fsRdWrPerm, &ref) != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    long count = (long)len;
    err = FSWrite(ref, &count, data);
    if (err == noErr && count == (long)len)
        err = SetEOF(ref, (long)len);
    else if (err == noErr)
        err = ioErr;
    OSErr cerr = FSClose(ref);
    if (err != noErr || cerr != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    err = FSMakeFSSpec(g_vref, g_dir, name, &dst);
    if (err == noErr) {
        if (FSpExchangeFiles(&tmp, &dst) == noErr) {
            FSpDelete(&tmp); /* now holds the old contents */
        } else {
            if (FSpDelete(&dst) != noErr) {
                FSpDelete(&tmp);
                return false;
            }
            if (FSpRename(&tmp, name) != noErr)
                return false;
        }
    } else if (err == fnfErr) {
        if (FSpRename(&tmp, name) != noErr)
            return false;
    } else {
        FSpDelete(&tmp);
        return false;
    }
    FlushVol(NULL, g_vref);
    return true;
}

static bool read_file(FileId id, char **data, size_t *len) { return storage_read_named(file_names[id], data, len); }
static void keep_aside(FileId id) { storage_keep_aside_named(file_names[id]); }
static bool write_file(FileId id, const char *data, size_t len)
{
    return storage_write_named(file_names[id], data, len);
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

static uint32_t generation(FileId id)
{
    AuBrowser *b = g_browser;
    switch (id) {
    case F_PREFS: return b->prefs_gen;
    case F_BOOKMARKS: return b->bookmarks.generation;
    case F_HISTORY: return b->history.generation;
    case F_SESSION: return b->session_gen + b->scroll_gen;
    case F_DOWNLOADS: return b->downloads.generation;
    default: return 0;
    }
}

static void serialize(FileId id, AuBuf *out)
{
    AuBrowser *b = g_browser;
    switch (id) {
    case F_PREFS: au_prefs_serialize(&b->prefs, out); break;
    case F_BOOKMARKS: au_bookmarks_serialize(&b->bookmarks, out); break;
    case F_HISTORY:
        au_history_expire(&b->history, mac_now(), b->prefs.history_days);
        au_history_serialize(&b->history, out);
        break;
    case F_SESSION: au_browser_session_serialize(b, out); break;
    case F_DOWNLOADS: au_downloads_serialize(&b->downloads, out); break;
    default: break;
    }
}

void storage_load_all(void)
{
    AuBrowser *b = g_browser;
    char *data;
    size_t len;
    b->history.utc_offset = au_mac_gmt_offset();
    if (read_file(F_PREFS, &data, &len)) {
        if (!au_prefs_parse(&b->prefs, data, len))
            keep_aside(F_PREFS);
        au_free(data);
    }
    if (read_file(F_BOOKMARKS, &data, &len)) {
        if (!au_bookmarks_parse(&b->bookmarks, data, len))
            keep_aside(F_BOOKMARKS);
        au_free(data);
    } else {
        /* A first run: a couple of starting points. */
        AuTime t = mac_now();
        au_bookmark_add(&b->bookmarks, NULL, NULL, "Aura start page", "about:home", t);
        AuBookmark *f = au_bookmark_add_folder(&b->bookmarks, NULL, NULL, "Mac OS 9", t);
        au_bookmark_add(&b->bookmarks, f, NULL, "Macintosh Garden", "http://macintoshgarden.org/", t);
        au_bookmark_add(&b->bookmarks, f, NULL, "Mac OS 9 Lives!", "http://macos9lives.com/", t);
        au_bookmark_add(&b->bookmarks, f, NULL, "FrogFind", "http://frogfind.com/", t);
    }
    if (read_file(F_HISTORY, &data, &len)) {
        if (!au_history_parse(&b->history, data, len))
            keep_aside(F_HISTORY);
        au_free(data);
        au_history_expire(&b->history, mac_now(), b->prefs.history_days);
    }
    if (read_file(F_DOWNLOADS, &data, &len)) {
        if (!au_downloads_parse(&b->downloads, data, len))
            keep_aside(F_DOWNLOADS);
        au_free(data);
    }
    for (int i = 0; i < F_COUNT; i++) {
        save_state[i].saved_gen = save_state[i].seen_gen = generation((FileId)i);
        save_state[i].pending = false;
    }
    saved_session_gen = b->session_gen;
}

/* ---- Unclean exits ----
 * A file "Running" exists while Aura runs: made at start, deleted after
 * the last save at quit. Found at start, it means the last run crashed
 * or was forced to quit (the out-of-memory handler leaves it too). */

static bool running_spec(FSSpec *spec)
{
    if (!g_ok)
        return false;
    OSErr err = FSMakeFSSpec(g_vref, g_dir, (ConstStr255Param) "\pRunning", spec);
    return err == noErr || err == fnfErr; /* fnfErr: valid, the file does not exist */
}

bool storage_begin_run(void)
{
    FSSpec spec;
    if (!running_spec(&spec))
        return false;
    FInfo fi;
    bool unclean = FSpGetFInfo(&spec, &fi) == noErr;
    if (!unclean && FSpCreate(&spec, 'AURA', 'TEXT', -1) == noErr)
        FlushVol(NULL, g_vref);
    return unclean;
}

void storage_end_run(void)
{
    FSSpec spec;
    if (running_spec(&spec)) {
        FSpDelete(&spec);
        FlushVol(NULL, g_vref);
    }
}

bool storage_session(AuSession *s)
{
    char *data;
    size_t len;
    memset(s, 0, sizeof *s);
    if (!read_file(F_SESSION, &data, &len))
        return false;
    if (!au_session_parse(s, data, len))
        keep_aside(F_SESSION);
    au_free(data);
    return s->nwindows > 0;
}

void storage_save_changed(bool force)
{
    uint32_t now = au_platform_time_ms();
    for (int i = 0; i < F_COUNT; i++) {
        SaveState *st = &save_state[i];
        uint32_t g = generation((FileId)i);
        if (g == st->saved_gen) {
            st->pending = false;
            continue;
        }
        if (!st->pending) {
            st->pending = true;
            st->first_change_ms = now;
        }
        /* The session is saved soon after tabs change, and every half
         * minute while only scroll positions do. */
        uint32_t delay = kSaveDelayMs;
        if (i == F_SESSION && g_browser->session_gen == saved_session_gen)
            delay = kScrollSaveDelayMs;
        if (!force && now - st->first_change_ms < delay)
            continue;
        AuBuf out;
        au_buf_init(&out);
        serialize((FileId)i, &out);
        if (write_file((FileId)i, out.data ? out.data : "", out.len)) {
            st->saved_gen = generation((FileId)i);
            if (i == F_SESSION)
                saved_session_gen = g_browser->session_gen;
            st->pending = false;
        } else {
            st->first_change_ms = now; /* try again after the delay */
        }
        au_buf_free(&out);
    }
    mac_net_save(force);
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
