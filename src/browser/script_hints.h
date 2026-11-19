/* Script hints: for each large script the browser ran, the functions
 * that were compiled at their first call, so that the next run of the
 * same script compiles them with it and parses them once
 * (js/js.h, AuJsHintStore; js/runtime.c, "Script hints").
 *
 * The store keeps the AU_SCRIPT_HINTS_SCRIPTS scripts used most
 * recently, each with at most AU_SCRIPT_HINTS_MAX offsets and all with
 * at most AU_SCRIPT_HINTS_TOTAL (a file of about 300 KB). It names
 * scripts by length and hash only, but those say which scripts were
 * run, so clearing the cache clears it too. */
#ifndef AURA_BROWSER_SCRIPT_HINTS_H
#define AURA_BROWSER_SCRIPT_HINTS_H

#include "../core/core.h"

struct AuJsHintStore; /* js/js.h */

#define AU_SCRIPT_HINTS_SCRIPTS 256
#define AU_SCRIPT_HINTS_MAX 8192
#define AU_SCRIPT_HINTS_TOTAL 65536

typedef struct AuScriptHints AuScriptHints;

AuScriptHints *au_script_hints_new(void);
void au_script_hints_free(AuScriptHints *h);
void au_script_hints_clear(AuScriptHints *h);
/* The interface the script runtime calls (AuJsSettings.hints). */
const struct AuJsHintStore *au_script_hints_store(AuScriptHints *h);
size_t au_script_hints_count(const AuScriptHints *h); /* scripts */

/* File format: "# Aura script hints 1", then one line per script, the
 * most recently used first: its length and hash in hexadecimal, then
 * its offsets as hexadecimal differences from the one before (the first
 * from 0), all separated by spaces. Parsing skips a damaged line and
 * returns false when the header is wrong. */
void au_script_hints_serialize(const AuScriptHints *h, AuBuf *out);
bool au_script_hints_parse(AuScriptHints *h, const char *data, size_t len);
bool au_script_hints_dirty(const AuScriptHints *h);
void au_script_hints_clear_dirty(AuScriptHints *h);

#endif
