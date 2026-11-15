/* The network stack's part of the developer tools' reports: the HTTP
 * cache's size and counts and the open connections per host, for
 * AuDevHostInfo (src/browser/devtools.h). Like page_fetcher.c, only the
 * plain structs of src/browser are used here. */
#include "cache.h"
#include "loader.h"
#include "../browser/devtools.h"

#include <stdio.h>
#include <string.h>

#define MAX_CONNS 32

void au_net_connections_text(const AuNet *net, AuBuf *out, int *total, int *busy)
{
    AuNetConnInfo *c = au_calloc(MAX_CONNS * sizeof *c);
    int n = au_net_connections(net, c, MAX_CONNS);
    *total = n;
    *busy = 0;
    if (n > MAX_CONNS)
        n = MAX_CONNS;
    for (int i = 0; i < n; i++)
        *busy += c[i].busy;
    /* One group per host and port, in list order; served < 0 marks a
     * connection already counted. */
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (c[i].served < 0)
            continue;
        int count = 0, nbusy = 0;
        for (int k = i; k < n; k++) {
            if (c[k].served < 0 || c[k].port != c[i].port || strcmp(c[k].host, c[i].host) != 0)
                continue;
            count++;
            nbusy += c[k].busy;
            if (k != i)
                c[k].served = -1;
        }
        au_buf_printf(out, "%s%s:%u x%d", first ? "" : ", ", c[i].host, (unsigned)c[i].port, count);
        if (nbusy)
            au_buf_printf(out, " (%d busy)", nbusy);
        first = false;
    }
    au_free(c);
}

void au_net_dev_info(AuNet *net, AuDevHostInfo *h, AuBuf *conn_text)
{
    h->have_net = true;
    AuHttpCache *cache = au_net_cache(net);
    if (cache) {
        AuCacheStats cs;
        au_cache_stats(cache, &cs);
        h->http_cache_kb = (uint32_t)(au_cache_bytes(cache) / 1024);
        h->http_cache_max_kb = (uint32_t)(au_cache_entry_limit(cache) / 1024 * 4);
        h->http_cache_entries = (uint32_t)au_cache_count(cache);
        h->cache_fresh = cs.fresh;
        h->cache_history = cs.history;
        h->cache_stale = cs.stale;
        h->cache_304 = cs.not_modified;
        h->cache_missed = cs.misses;
        h->cache_stored = cs.stored;
        h->cache_disk = cs.from_storage;
    }
    au_buf_clear(conn_text);
    au_net_connections_text(net, conn_text, &h->connections, &h->connections_busy);
    h->connection_text = au_buf_cstr(conn_text);
}
