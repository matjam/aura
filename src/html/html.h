/* Aura HTML parser: bytes to DOM, following the WHATWG HTML Standard
 * parsing algorithm (tokenizer, tree construction, fragment parsing), and
 * the HTML fragment serialization algorithm.
 *
 * To parse a whole buffer, use au_html_parse. To parse as data arrives,
 * create a parser with au_html_parser_new, pass each chunk to
 * au_html_parser_feed and end with au_html_parser_finish.
 *
 * Scripts. When the tree builder processes a </script> end tag it calls
 * opts->script_ready with the script element. While that callback runs,
 * the insertion point is just before the next input character, so
 * au_html_parser_write (document.write) inserts there and parses the
 * written text at once. To block parsing on the script (an external script
 * still loading), call au_html_parser_pause from the callback; input fed
 * afterwards is buffered. When the script has run, call
 * au_html_parser_resume. For a script run after a pause, bracket its
 * execution with au_html_parser_begin_script / au_html_parser_end_script so
 * document.write has an insertion point. */
#ifndef AURA_HTML_H
#define AURA_HTML_H

#include "../dom/dom.h"

typedef struct AuHtmlParser AuHtmlParser;

typedef enum {
    AU_HTML_ENC_SNIFF,        /* BOM, else the default (UTF-8 for now) */
    AU_HTML_ENC_UTF8,
    AU_HTML_ENC_UTF16LE,
    AU_HTML_ENC_UTF16BE,
    AU_HTML_ENC_WINDOWS_1252  /* also what the labels iso-8859-1 and us-ascii mean */
} AuHtmlEncoding;

typedef struct AuHtmlOptions {
    /* Scripting flag. When true, <noscript> content is raw text. */
    bool scripting;
    /* Encoding of the bytes. A BOM overrides anything but SNIFF's default. */
    AuHtmlEncoding encoding;
    /* Called when a script element's end tag has been processed. */
    void (*script_ready)(void *ctx, AuHtmlParser *p, AuElement *script);
    /* Called for each tokenizer parse error, with its WHATWG error code
     * (for example "eof-in-tag") and the 1-based position. Optional. */
    void (*parse_error)(void *ctx, const char *code, int line, int col);
    void *ctx;
} AuHtmlOptions;

/* Parses a whole document. opts may be NULL. The caller owns the result
 * (release it with au_node_unref). */
AuDocument *au_html_parse(const char *bytes, size_t len, const AuHtmlOptions *opts);

/* ---- Incremental parsing ---- */
AuHtmlParser *au_html_parser_new(const AuHtmlOptions *opts);
void au_html_parser_feed(AuHtmlParser *p, const char *bytes, size_t len);
/* Marks the end of input and parses what remains (unless paused, in which
 * case parsing completes on au_html_parser_resume). */
void au_html_parser_finish(AuHtmlParser *p);
bool au_html_parser_done(const AuHtmlParser *p);
/* The document being built; the parser keeps a reference until freed. */
AuDocument *au_html_parser_document(const AuHtmlParser *p);
void au_html_parser_free(AuHtmlParser *p);

/* ---- Script support ---- */
void au_html_parser_pause(AuHtmlParser *p);
void au_html_parser_resume(AuHtmlParser *p);
bool au_html_parser_paused(const AuHtmlParser *p);
void au_html_parser_begin_script(AuHtmlParser *p);
void au_html_parser_end_script(AuHtmlParser *p);
/* document.write: inserts UTF-8 text at the insertion point and, unless
 * parsing is paused, parses it. Returns false if there is no insertion
 * point (the caller then follows document.open's rules). */
bool au_html_parser_write(AuHtmlParser *p, const char *utf8, size_t len);

/* ---- Fragments (innerHTML) ----
 * Parses bytes (UTF-8) with context as the context element and returns a
 * DocumentFragment in context's document, owned by the caller. */
AuNode *au_html_parse_fragment(AuElement *context, const char *bytes, size_t len, const AuHtmlOptions *opts);

/* ---- Serialization ----
 * Appends the HTML fragment serialization of node's children (for a
 * template, its contents) to out. scripting decides how <noscript> text
 * is escaped. */
void au_html_serialize(const AuNode *node, AuBuf *out, bool scripting);

#endif
