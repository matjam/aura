/* AuNetOps on Mac OS 9: Open Transport TCP endpoints and DNS.
 *
 * Every Open Transport call is made from the main task, asynchronously:
 * endpoints and Internet services providers are opened in asynchronous
 * mode with a notifier, and data moves with non-blocking OTSnd and OTRcv
 * when the engine pumps. The notifiers run at deferred task time and only
 * record which completion arrived and its result in the slot they were
 * given; the main task acts on it on its next call. No Open Transport
 * call is made from a notifier, an MP task or a Time Manager task.
 *
 * Open Transport is IPv4 only here; an IPv6 literal fails to resolve.
 *
 * Use: au_ot_net_new once after the Toolbox is set up, then pass
 * au_ot_net_ops to au_net_new. Call au_ot_net_idle once per pass of the
 * event loop (it finishes closing providers whose open was still pending
 * when they were closed), and sleep briefly in WaitNextEvent while
 * au_ot_net_busy is true. au_ot_net_free after au_net_free. */
#ifndef AURA_NET_OT_H
#define AURA_NET_OT_H

#include "entropy.h"
#include "platform/platform.h"

typedef struct AuOtNet AuOtNet;

/* NULL when Open Transport or its TCP/IP stack is missing or will not
 * start; *err (optional) then gets the Open Transport error, or 1 for
 * "not installed". The entropy pool is borrowed and must outlive the
 * AuOtNet. */
AuOtNet *au_ot_net_new(AuMacEntropy *entropy, long *err);
void au_ot_net_free(AuOtNet *n);
const AuNetOps *au_ot_net_ops(AuOtNet *n);
void au_ot_net_idle(AuOtNet *n);
/* A lookup, connection attempt or open connection exists. */
bool au_ot_net_busy(const AuOtNet *n);

/* Counters for diagnostics. */
typedef struct AuOtNetStats {
    unsigned lookups, lookups_failed;
    unsigned connects, connects_failed;
    unsigned long bytes_sent, bytes_received;
    long last_error; /* the last Open Transport error seen */
} AuOtNetStats;
void au_ot_net_stats(const AuOtNet *n, AuOtNetStats *out);

#endif
