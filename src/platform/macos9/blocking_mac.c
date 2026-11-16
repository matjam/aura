/* Content blocking in the Mac OS 9 application: the blocklist set the
 * loader checks, the update manager, the Blocklists folder and the
 * memory the lists live in.
 *
 * A compiled list is one block of 0.5 to several MB. It comes from
 * temporary memory (TempNewHandle, locked), as font data does, so the
 * application heap keeps its room for pages; when temporary memory has
 * none, from the heap without the out-of-memory handler (au_try_alloc).
 * When neither has room the list is not loaded, blocking for that list
 * stays off and the Preferences pane says so.
 *
 * Files: "Blocklists" in the Aura preferences folder holds one compiled
 * file per list (named by the list's id) and "Index". A file is written
 * as "<name>.tmp" and renamed over the old one.
 *
 * Downloads start from mac_blocking_idle only, at least kStartDelayMs
 * after launch and while nothing loads; everything runs on the main
 * task, through the same AuNet as pages. */
#include "blocking_mac.h"

#include "clock_mac.h"

#include "browser/netlog.h"

#include <stdio.h>
#include <string.h>

enum { kStartDelayMs = 4000 };

static struct {
    AuBlocklist *bl;
    AuBlockManager *mgr;
    short vref;
    long dir;
    bool have_dir;
    uint32_t started_ms;
    bool working;
    unsigned long temp_bytes, heap_bytes; /* list memory by where it came from */
} B;

/* ---- Memory ---- */

typedef struct BlockHead {
    Handle h;      /* the temporary memory handle, or NULL for a heap block */
    uint32_t size;
} BlockHead;

static void *bm_alloc(void *ctx, size_t n)
{
    (void)ctx;
    OSErr err = noErr;
    Handle h = TempNewHandle((Size)(n + sizeof(BlockHead)), &err);
    if (h && err == noErr) {
        HLock(h);
        BlockHead *bh = (BlockHead *)*h;
        bh->h = h;
        bh->size = (uint32_t)n;
        B.temp_bytes += n;
        return bh + 1;
    }
    BlockHead *bh = au_try_alloc(n + sizeof(BlockHead));
    if (!bh)
        return NULL;
    bh->h = NULL;
    bh->size = (uint32_t)n;
    B.heap_bytes += n;
    return bh + 1;
}

static void bm_release(void *ctx, void *p)
{
    (void)ctx;
    BlockHead *bh = (BlockHead *)p - 1;
    if (bh->h) {
        B.temp_bytes -= bh->size;
        DisposeHandle(bh->h);
    } else {
        B.heap_bytes -= bh->size;
        au_free(bh);
    }
}

static const AuBlockMem block_mem = {NULL, bm_alloc, bm_release};

/* ---- Files ---- */

static bool spec_of(const char *name, FSSpec *spec)
{
    Str255 p;
    size_t n = strlen(name);
    if (!B.have_dir || n > 31)
        return false;
    p[0] = (unsigned char)n;
    memcpy(p + 1, name, n);
    OSErr err = FSMakeFSSpec(B.vref, B.dir, p, spec);
    return err == noErr || err == fnfErr;
}

static bool bs_read(void *ctx, const char *name, const AuBlockMem *mem, uint8_t **data, size_t *len)
{
    (void)ctx;
    FSSpec s;
    short ref;
    if (!spec_of(name, &s) || FSpOpenDF(&s, fsRdPerm, &ref) != noErr)
        return false;
    long eof = 0;
    uint8_t *buf = NULL;
    if (GetEOF(ref, &eof) == noErr && eof > 0 && eof < 64L * 1024 * 1024) {
        buf = mem && mem->alloc ? mem->alloc(mem->ctx, (size_t)eof) : au_try_alloc((size_t)eof);
        if (buf) {
            long count = eof;
            OSErr err = FSRead(ref, &count, buf);
            if (!((err == noErr || err == eofErr) && count == eof)) {
                if (mem && mem->alloc)
                    mem->release(mem->ctx, buf);
                else
                    au_free(buf);
                buf = NULL;
            }
        }
    }
    FSClose(ref);
    if (!buf)
        return false;
    *data = buf;
    *len = (size_t)eof;
    return true;
}

static bool bs_write(void *ctx, const char *name, const void *data, size_t len)
{
    (void)ctx;
    char tname[40];
    snprintf(tname, sizeof tname, "%s.tmp", name);
    FSSpec tmp, dst;
    if (strlen(name) > 27 || !spec_of(tname, &tmp) || !spec_of(name, &dst))
        return false;
    FSpDelete(&tmp);
    if (FSpCreate(&tmp, 'AURA', 'AuBL', smSystemScript) != noErr)
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
    p[0] = (unsigned char)strlen(name);
    memcpy(p + 1, name, p[0]);
    if (FSpRename(&tmp, p) != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    FlushVol(NULL, B.vref);
    return true;
}

static void bs_remove(void *ctx, const char *name)
{
    (void)ctx;
    FSSpec s;
    if (spec_of(name, &s))
        FSpDelete(&s);
}

/* ---- Life cycle ---- */

static void changed_by_model(void *ctx)
{
    (void)ctx;
    mac_blocking_prefs_changed();
}

void mac_blocking_init(AuNet *net, AuNetLoader *loader)
{
    B.have_dir = storage_subfolder("Blocklists", &B.vref, &B.dir);
    B.bl = au_blocklist_new();
    AuBlockStore store = {NULL, bs_read, bs_write, bs_remove};
    B.mgr = au_block_manager_new(net, B.bl, &store, &block_mem);
    B.started_ms = au_mac_clock_ms();
    au_block_manager_apply_prefs(B.mgr, &g_browser->prefs);
    au_net_loader_set_blocklist(loader, B.bl);
    g_browser->blocking_changed = changed_by_model;
    g_browser->blocking_ctx = NULL;
}

void mac_blocking_idle(bool loading)
{
    if (!B.mgr)
        return;
    bool early = au_mac_clock_ms() - B.started_ms < kStartDelayMs;
    B.working = au_block_manager_idle(B.mgr, loading || early);
}

bool mac_blocking_busy(void) { return B.mgr && B.working; }

void mac_blocking_prefs_changed(void)
{
    if (B.mgr)
        au_block_manager_apply_prefs(B.mgr, &g_browser->prefs);
}

void mac_blocking_quit(void)
{
    if (!B.mgr)
        return;
    g_browser->blocking_changed = NULL;
    au_block_manager_free(B.mgr);
    au_blocklist_free(B.bl);
    memset(&B, 0, sizeof B);
}

AuBlockManager *mac_blocking_manager(void) { return B.mgr; }

void mac_blocking_status_text(AuBuf *out)
{
    au_buf_puts(out, "\nContent blocking\n");
    if (!B.bl) {
        au_report_kv(out, "State", "not started");
        return;
    }
    AuBlockStats s;
    AuBlockCompileStats c;
    au_blocklist_stats(B.bl, &s);
    au_block_manager_compile_stats(B.mgr, &c);
    au_report_kv(out, "State", "%s", au_blocklist_enabled(B.bl) ? "on" : "off");
    AuBuf n;
    au_buf_init(&n);
    au_put_count(&n, (unsigned long)s.entries);
    au_report_kv(out, "Lists", "%lu list%s, %s domains", (unsigned long)s.lists, s.lists == 1 ? "" : "s", au_buf_cstr(&n));
    au_buf_free(&n);
    au_report_kv(out, "Memory", "%lu KB   (%lu KB temporary memory, %lu KB heap)", (unsigned long)(s.bytes / 1024),
                 B.temp_bytes / 1024, B.heap_bytes / 1024);
    au_report_kv(out, "Requests", "%lu checked, %lu blocked, %lu pages stopped", (unsigned long)s.checked,
                 (unsigned long)s.blocked, (unsigned long)s.navigations);
    if (c.id[0])
        au_report_kv(out, "Last list compiled", "%s: %lu KB of text, fetch and parse %lu ms, merge %lu ms in %lu passes", c.id,
                     (unsigned long)(c.text_bytes / 1024), (unsigned long)c.download_ms, (unsigned long)c.merge_ms,
                     (unsigned long)c.merge_calls);
}
