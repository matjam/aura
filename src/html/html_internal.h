/* Internal interfaces of the HTML parser: tokenizer, tree builder, input
 * decoding and the entity table. Not for use outside src/html and
 * tests/html. */
#ifndef AURA_HTML_INTERNAL_H
#define AURA_HTML_INTERNAL_H

#include "html.h"

/* ---- Named character references (entities.c) ---- */

/* Returns the index of name (without '&' and ';') or -1. */
int au_html_entity_find(const char *name, size_t len);
/* True if entry idx also matches without a trailing semicolon. */
bool au_html_entity_legacy(int idx);
/* Stores the one or two code points of entry idx; returns how many. */
int au_html_entity_codepoints(int idx, uint32_t cp[2]);
/* Longest name in the table. */
size_t au_html_entity_max_len(void);

/* ---- Input decoding (input.c) ----
 * Converts bytes in some encoding to UTF-8, replacing malformed sequences
 * with U+FFFD, and normalizes newlines (CR LF and lone CR become LF). The
 * output is always valid UTF-8. A chunk may end in the middle of a
 * character; the decoder keeps the partial bytes for the next chunk. */
typedef struct HtmlDecoder {
    uint8_t enc;          /* AuHtmlEncoding in use; SNIFF until decided */
    uint8_t fallback;     /* encoding to use when there is no BOM */
    bool last_cr;         /* the previous character was a CR */
    uint16_t nhold;       /* bytes held until the BOM and the prescan decide */
    unsigned char hold[1024];
    /* UTF-8 decoder state (WHATWG Encoding Standard). */
    uint8_t need, seen, lower, upper;
    uint32_t cp;
    /* UTF-16 decoder state: lead byte (-1: none) and lead surrogate. */
    int lead_byte;
    uint32_t lead_surrogate;
} HtmlDecoder;

void html_decoder_init(HtmlDecoder *d, AuHtmlEncoding enc);
/* Appends the decoded form of bytes to out. When last is true, any
 * partial character left over is flushed as U+FFFD. */
void html_decode(HtmlDecoder *d, const char *bytes, size_t len, bool last, AuBuf *out);
/* Appends UTF-8 text to out with newlines normalized (document.write). */
void html_normalize_newlines(const char *s, size_t len, AuBuf *out);

/* ---- Tokens ---- */

enum {
    HTML_TOK_DOCTYPE,
    HTML_TOK_START,
    HTML_TOK_END,
    HTML_TOK_COMMENT,
    HTML_TOK_CHARS,
    HTML_TOK_PI,
    HTML_TOK_EOF
};

typedef struct HtmlAttr {
    AuAtom name;   /* lowercase as tokenized; adjusted by the tree builder */
    uint8_t ns;    /* AuAttrNamespace, set by the tree builder */
    uint32_t voff; /* value: vals + voff, vlen bytes */
    uint32_t vlen;
} HtmlAttr;

/* A token is valid only during the emit callback; its strings point into
 * tokenizer buffers. The tree builder may rewrite name and attribute
 * names in place. */
typedef struct HtmlToken {
    uint8_t type;
    bool self_closing;
    bool force_quirks;
    bool has_name, has_public, has_system; /* DOCTYPE: present, not missing */
    AuAtom name;                 /* tag name; PI target */
    HtmlAttr *attrs;
    uint32_t nattrs;
    const char *vals;            /* attribute value storage */
    const char *data;            /* characters, comment, PI data, DOCTYPE name */
    size_t len;
    const char *pub, *sys;       /* DOCTYPE identifiers */
    size_t publen, syslen;
} HtmlToken;

/* ---- Tokenizer (tokenizer.c) ---- */

enum {
    HTML_STATE_DATA,
    HTML_STATE_RCDATA,
    HTML_STATE_RAWTEXT,
    HTML_STATE_SCRIPT_DATA,
    HTML_STATE_PLAINTEXT,
    HTML_STATE_CDATA_SECTION,
    HTML_STATE_COUNT_PUBLIC /* the states above may be set from outside */
};

typedef struct HtmlTokenizer {
    /* Input: valid, newline-normalized UTF-8. Bytes before pos are
     * consumed. */
    char *buf;
    size_t len, cap, pos;
    size_t checked;  /* input-stream errors reported up to here */
    unsigned limit_ins; /* nonzero: input from ins on is not yet available
                         * (parsing document.write text up to the
                         * insertion point) */
    bool eof;        /* no more input will be appended */
    bool pause;      /* stop before the next step (parser pause flag) */
    bool done;       /* the EOF token has been emitted */
    int line, col;   /* position of buf[pos] */
    int pi_line, pi_col; /* where the current processing instruction began */

    size_t ins;      /* insertion point (document.write), or (size_t)-1 */

    uint8_t state, return_state;
    uint8_t text_state;    /* state an end tag attempt falls back to */
    bool doctype_has_name;
    bool end_tag;          /* the tag being built is an end tag */
    bool self_closing;
    bool force_quirks, has_public, has_system;
    bool attr_live;        /* the current attribute is kept (not a duplicate) */
    uint32_t charref;
    AuAtom last_start_tag;

    AuBuf name;   /* tag name, DOCTYPE name, PI target */
    AuBuf attrname;
    AuBuf vals;   /* attribute values of the current tag */
    AuBuf data;   /* comment data, PI data */
    AuBuf pub, sys;
    AuBuf tmp;    /* the temporary buffer */
    AuBuf chars;  /* characters not yet emitted */
    HtmlAttr *attrs;
    uint32_t nattrs, attr_cap;
    AuAtom *attr_set;      /* hash set of attribute names, for long tags */
    uint32_t attr_set_size;
    bool attr_set_live;    /* the set holds the current tag's names */

    /* Sink: the tree builder, or a test harness. */
    void (*emit)(void *sink, HtmlToken *tok);
    /* True if the adjusted current node is a non-HTML element. */
    bool (*cdata_ok)(void *sink);
    void *sink;
    void (*error)(void *ctx, const char *code, int line, int col);
    void *error_ctx;
} HtmlTokenizer;

void html_tok_init(HtmlTokenizer *t);
void html_tok_free(HtmlTokenizer *t);
/* Appends decoded input. */
void html_tok_append(HtmlTokenizer *t, const char *utf8, size_t len);
/* Inserts decoded input at offset at (an insertion point, at >= pos). */
void html_tok_insert(HtmlTokenizer *t, size_t at, const char *utf8, size_t len);
/* Runs until input runs out, the EOF token is emitted, or pause is set. */
void html_tok_run(HtmlTokenizer *t);
/* Sets one of the public states (tree builder and tests). */
void html_tok_set_state(HtmlTokenizer *t, int state);
void html_tok_set_last_start_tag(HtmlTokenizer *t, AuAtom tag);

/* ---- Tree builder (treebuilder.c) ---- */

typedef struct HtmlTreeBuilder HtmlTreeBuilder;

HtmlTreeBuilder *html_tb_new(AuDocument *doc, HtmlTokenizer *tok, bool scripting);
/* Sets up the fragment case for context (in context's document). */
void html_tb_set_fragment_context(HtmlTreeBuilder *tb, AuElement *context);
/* <template shadowrootmode> makes shadow roots (AuHtmlOptions). */
void html_tb_set_declarative_shadow(HtmlTreeBuilder *tb, bool on);
/* The html element of the fragment case. */
AuNode *html_tb_fragment_root(HtmlTreeBuilder *tb);
void html_tb_free(HtmlTreeBuilder *tb);
void html_tb_emit(void *tb, HtmlToken *tok);
bool html_tb_cdata_ok(void *tb);
/* Serializer element classes (HTML namespace tags). */
bool html_serializes_void(AuAtom tag);
bool html_serializes_rawtext(AuAtom tag);
void html_tb_set_script_hook(HtmlTreeBuilder *tb,
                             void (*hook)(void *ctx, AuElement *script), void *ctx);

#endif
