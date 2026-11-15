/* Internet Config: helper applications for addresses Aura does not open
 * itself (mailto:, ftp:, news: and others, through ICLaunchURL), the
 * user's download folder, the file name to type and creator mapping for
 * downloads, and making Aura the default web browser (the http and
 * https helpers).
 *
 * Mac OS 9.2.2 provides Internet Config as a component (type 'PREF',
 * subtype 'ICAp'), but not the InternetConfigLib shared library that
 * Retro68's stub library would link against (GetSharedLibrary returns
 * cfragNoLibraryErr in the stock guest). So Aura calls the component
 * itself, the way Apple's glue did: OpenDefaultComponent, then each
 * call through CallComponentUPP with a parameter block. Every selector
 * used is checked with kComponentCanDoSelect first. Each operation opens
 * and closes its own instance: the calls are rare (a click, a download)
 * and an instance held open would keep stale preferences.
 *
 * Selectors and prototypes follow Apple's InternetConfig.h. */
#include "mac.h"

#include "internetconfig.h"

#include <string.h>

/* The layouts the 68K alignment must give (Apple's sizes). */
typedef char ic_check_app[sizeof(ICAppSpec) == 68 ? 1 : -1];
typedef char ic_check_file[sizeof(ICFileSpec) == 106 ? 1 : -1];
typedef char ic_check_map[sizeof(ICMapEntry) == 22 + 5 * 256 ? 1 : -1];

enum {
    kICComponentType = 'PREF',
    kICComponentSubType = 'ICAp',
    kComponentCanDoSelect = -3,
    kICCStart = 0,
    kICCStop = 1,
    kICCFindConfigFile = 2,
    kICCGetPref = 6,
    kICCSetPref = 7,
    kICCLaunchURL = 17,
    kICCMapFilename = 24,
    /* CallComponentUPP's routine descriptor: Pascal, a 4-byte result,
     * one 4-byte parameter (the parameter block). */
    kCallComponentProcInfo = 0x000000F0
};

static int ic_state; /* 0 not checked, 1 usable, -1 not */

/* One component call. params holds the call's parameters as Pascal
 * pushes them (the last one first, at the lowest address), size bytes;
 * the instance follows them, as in Apple's ComponentParameters. */
static long cc(ComponentInstance ci, short what, const void *params, unsigned size)
{
    union {
        long align;
        unsigned char b[4 + 32 + 4];
    } pb;
    if (size > 32)
        return paramErr;
    pb.b[0] = 0;                    /* flags */
    pb.b[1] = (unsigned char)size; /* paramSize */
    pb.b[2] = (unsigned char)((unsigned short)what >> 8);
    pb.b[3] = (unsigned char)what;
    memcpy(pb.b + 4, params, size);
    memcpy(pb.b + 4 + size, &ci, sizeof ci);
    return CallUniversalProc(CallComponentUPP, kCallComponentProcInfo, &pb);
}

/* Parameter blocks are built with these: 4-byte values and 2-byte
 * values in push order. */
typedef struct {
    unsigned char b[32];
    unsigned n;
} Params;

static void p_long(Params *p, long v)
{
    memcpy(p->b + p->n, &v, 4);
    p->n += 4;
}

static void p_ptr(Params *p, const void *v)
{
    memcpy(p->b + p->n, &v, 4);
    p->n += 4;
}

static void p_short(Params *p, short v)
{
    memcpy(p->b + p->n, &v, 2);
    p->n += 2;
}

static bool can_do(ComponentInstance ci, short sel)
{
    Params p = {{0}, 0};
    p_short(&p, sel);
    return cc(ci, kComponentCanDoSelect, p.b, p.n) != 0;
}

static void ic_close(ComponentInstance ci)
{
    cc(ci, kICCStop, NULL, 0);
    CloseComponent(ci);
}

static ComponentInstance ic_open(void)
{
    if (ic_state < 0)
        return NULL;
    ComponentDescription cd;
    memset(&cd, 0, sizeof cd);
    cd.componentType = kICComponentType;
    cd.componentSubType = kICComponentSubType;
    if (!FindNextComponent(NULL, &cd)) {
        ic_state = -1;
        return NULL;
    }
    ComponentInstance ci = OpenDefaultComponent(kICComponentType, kICComponentSubType);
    if (!ci) {
        ic_state = -1;
        return NULL;
    }
    if (ic_state == 0) {
        static const short sels[] = {kICCStart, kICCStop, kICCFindConfigFile, kICCGetPref, kICCSetPref,
                                     kICCLaunchURL, kICCMapFilename};
        ic_state = 1;
        for (size_t i = 0; i < sizeof sels / sizeof sels[0]; i++)
            if (!can_do(ci, sels[i]))
                ic_state = -1;
        if (ic_state < 0) {
            CloseComponent(ci);
            return NULL;
        }
    }
    Params p = {{0}, 0};
    p_long(&p, 'AURA'); /* ICCStart(ci, signature) */
    if (cc(ci, kICCStart, p.b, p.n) != noErr) {
        CloseComponent(ci);
        return NULL;
    }
    /* ICCFindConfigFile(ci, 0, NULL): needed before 2.0, harmless after. */
    p.n = 0;
    p_ptr(&p, NULL);
    p_short(&p, 0);
    if (cc(ci, kICCFindConfigFile, p.b, p.n) != noErr) {
        ic_close(ci);
        return NULL;
    }
    return ci;
}

/* ICCGetPref(ci, key, &attr, buf, &size) */
static long get_pref(ComponentInstance ci, ConstStr255Param key, void *buf, long *size)
{
    ICAttr attr = 0;
    Params p = {{0}, 0};
    p_ptr(&p, size);
    p_ptr(&p, buf);
    p_ptr(&p, &attr);
    p_ptr(&p, key);
    return cc(ci, kICCGetPref, p.b, p.n);
}

/* ICCSetPref(ci, key, attr, buf, size) */
static long set_pref(ComponentInstance ci, ConstStr255Param key, const void *buf, long size)
{
    Params p = {{0}, 0};
    p_long(&p, size);
    p_ptr(&p, buf);
    p_long(&p, (long)kAuICAttrNoChange);
    p_ptr(&p, key);
    return cc(ci, kICCSetPref, p.b, p.n);
}

bool mac_ic_available(void)
{
    ComponentInstance ci = ic_open();
    if (!ci)
        return false;
    ic_close(ci);
    return true;
}

bool mac_ic_launch_url(const char *url)
{
    ComponentInstance ci = ic_open();
    if (!ci)
        return false;
    size_t n;
    char *r = mac_utf8_to_roman(url, &n);
    long start = 0, end = (long)n;
    /* ICCLaunchURL(ci, hint, data, len, &selStart, &selEnd) */
    Params p = {{0}, 0};
    p_ptr(&p, &end);
    p_ptr(&p, &start);
    p_long(&p, (long)n);
    p_ptr(&p, r);
    p_ptr(&p, "\p");
    long err = cc(ci, kICCLaunchURL, p.b, p.n);
    au_free(r);
    ic_close(ci);
    return err == noErr;
}

static void helper_key(const char *scheme, Str255 key)
{
    size_t n = strlen(scheme);
    if (n > 200)
        n = 200;
    memcpy(key + 1, "Helper\xA5", 7); /* "Helper" and a bullet */
    memcpy(key + 8, scheme, n);
    key[0] = (unsigned char)(7 + n);
}

static bool get_helper(ComponentInstance ci, const char *scheme, ICAppSpec *out)
{
    Str255 key;
    helper_key(scheme, key);
    long size = sizeof *out;
    memset(out, 0, sizeof *out);
    return get_pref(ci, key, out, &size) == noErr && size >= 4;
}

bool mac_ic_has_helper(const char *scheme)
{
    ComponentInstance ci = ic_open();
    if (!ci)
        return false;
    ICAppSpec spec;
    bool ok = get_helper(ci, scheme, &spec) && spec.fCreator != 0 && spec.fCreator != 0x3F3F3F3F; /* '????' */
    ic_close(ci);
    return ok;
}

bool mac_ic_is_default_browser(void)
{
    ComponentInstance ci = ic_open();
    if (!ci)
        return false;
    ICAppSpec spec;
    bool ok = get_helper(ci, "http", &spec) && spec.fCreator == 'AURA';
    ic_close(ci);
    return ok;
}

bool mac_ic_make_default_browser(void)
{
    ComponentInstance ci = ic_open();
    if (!ci)
        return false;
    ICAppSpec spec;
    memset(&spec, 0, sizeof spec);
    spec.fCreator = 'AURA';
    memcpy(spec.name, "\pAura", 5);
    static const char *const schemes[] = {"http", "https", "file"};
    long err = noErr;
    for (int i = 0; i < 3 && err == noErr; i++) {
        Str255 key;
        helper_key(schemes[i], key);
        err = set_pref(ci, key, &spec, sizeof spec);
    }
    ic_close(ci);
    return err == noErr;
}

bool mac_ic_download_folder(FSSpec *out)
{
    ComponentInstance ci = ic_open();
    if (!ci)
        return false;
    /* The record ends in a variable-length alias: read it into a buffer
     * big enough for any. */
    union {
        ICFileSpec spec;
        char raw[1024];
    } *buf = (void *)NewPtrClear(sizeof *buf);
    if (!buf) {
        ic_close(ci);
        return false;
    }
    long size = sizeof *buf;
    long err = get_pref(ci, (ConstStr255Param) "\pDownloadFolder", buf, &size);
    ic_close(ci);
    /* kAuICPrefNotFoundErr when the user never chose a folder. */
    bool ok = err == noErr && size >= (long)sizeof(ICFileSpec);
    if (ok)
        *out = buf->spec.fss;
    DisposePtr((Ptr)buf);
    return ok;
}

bool mac_ic_map_filename(const char *name, MacFileMap *out)
{
    ComponentInstance ci = ic_open();
    if (!ci)
        return false;
    Str255 fname;
    mac_pstr(name, fname);
    ICMapEntry *e = (ICMapEntry *)NewPtrClear(sizeof(ICMapEntry));
    if (!e) {
        ic_close(ci);
        return false;
    }
    /* ICCMapFilename(ci, filename, &entry) */
    Params p = {{0}, 0};
    p_ptr(&p, e);
    p_ptr(&p, fname);
    long err = cc(ci, kICCMapFilename, p.b, p.n);
    if (err == noErr) {
        out->type = e->fileType;
        out->creator = e->fileCreator;
        size_t n = e->MIMEType[0] < sizeof out->mime - 1 ? e->MIMEType[0] : sizeof out->mime - 1;
        memcpy(out->mime, e->MIMEType + 1, n);
        out->mime[n] = 0;
        out->binary = (e->flags & 1) != 0; /* ICmap_binary_mask */
    }
    DisposePtr((Ptr)e);
    ic_close(ci);
    return err == noErr;
}
