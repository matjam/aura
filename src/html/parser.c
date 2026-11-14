/* The public parser API: ties the decoder, tokenizer and tree builder
 * together, and manages the insertion point for document.write.
 *
 * Insertion point. While a script runs (script_ready callback, or between
 * au_html_parser_begin_script and _end_script), tok.ins is the offset
 * just before the next input character. au_html_parser_write inserts
 * there, moves the insertion point past the new text and, unless parsing
 * is paused, tokenizes up to the insertion point (tok.limit_ins). Outer
 * insertion points of nested scripts are saved in ins_stack and shifted
 * when text is inserted before them. */
#include "html_internal.h"

#include <string.h>

struct AuHtmlParser {
    AuHtmlOptions opts;
    AuDocument *doc;
    HtmlDecoder dec;
    HtmlTokenizer tok;
    HtmlTreeBuilder *tb;
    AuBuf tmp;
    bool finished;
    bool paused;
    size_t *ins_stack;
    uint32_t nins, ins_cap;
};

static void run(AuHtmlParser *p)
{
    p->tok.pause = p->paused;
    if (!p->paused)
        html_tok_run(&p->tok);
}

static void save_ins(AuHtmlParser *p)
{
    if (p->nins == p->ins_cap) {
        p->ins_cap = p->ins_cap ? p->ins_cap * 2 : 4;
        p->ins_stack = au_realloc(p->ins_stack, p->ins_cap * sizeof *p->ins_stack);
    }
    p->ins_stack[p->nins++] = p->tok.ins;
    p->tok.ins = p->tok.pos;
}

static void restore_ins(AuHtmlParser *p)
{
    if (p->nins)
        p->tok.ins = p->ins_stack[--p->nins];
}

static void on_script(void *ctx, AuElement *script)
{
    AuHtmlParser *p = ctx;
    if (!p->opts.script_ready)
        return;
    save_ins(p);
    p->opts.script_ready(p->opts.ctx, p, script);
    restore_ins(p);
    p->tok.pause = p->paused;
}

AuHtmlParser *au_html_parser_new(const AuHtmlOptions *opts)
{
    AuHtmlParser *p = au_calloc(sizeof *p);
    if (opts)
        p->opts = *opts;
    p->doc = au_document_new();
    html_decoder_init(&p->dec, (AuHtmlEncoding)p->opts.encoding);
    html_tok_init(&p->tok);
    p->tb = html_tb_new(p->doc, &p->tok, p->opts.scripting);
    html_tb_set_script_hook(p->tb, on_script, p);
    p->tok.emit = html_tb_emit;
    p->tok.cdata_ok = html_tb_cdata_ok;
    p->tok.sink = p->tb;
    p->tok.error = p->opts.parse_error;
    p->tok.error_ctx = p->opts.ctx;
    au_buf_init(&p->tmp);
    return p;
}

void au_html_parser_feed(AuHtmlParser *p, const char *bytes, size_t len)
{
    if (p->finished)
        return;
    p->tmp.len = 0;
    html_decode(&p->dec, bytes, len, false, &p->tmp);
    html_tok_append(&p->tok, p->tmp.data, p->tmp.len);
    if (!p->nins)
        run(p);
}

void au_html_parser_finish(AuHtmlParser *p)
{
    if (p->finished)
        return;
    p->tmp.len = 0;
    html_decode(&p->dec, "", 0, true, &p->tmp);
    html_tok_append(&p->tok, p->tmp.data, p->tmp.len);
    p->finished = true;
    p->tok.eof = true;
    if (!p->nins)
        run(p);
}

bool au_html_parser_done(const AuHtmlParser *p)
{
    return p->tok.done;
}

AuDocument *au_html_parser_document(const AuHtmlParser *p)
{
    return p->doc;
}

void au_html_parser_free(AuHtmlParser *p)
{
    if (!p)
        return;
    html_tb_free(p->tb);
    html_tok_free(&p->tok);
    au_buf_free(&p->tmp);
    au_free(p->ins_stack);
    au_node_unref(&p->doc->node);
    au_free(p);
}

void au_html_parser_pause(AuHtmlParser *p)
{
    p->paused = true;
    p->tok.pause = true;
}

void au_html_parser_resume(AuHtmlParser *p)
{
    p->paused = false;
    if (!p->nins)
        run(p);
}

bool au_html_parser_paused(const AuHtmlParser *p)
{
    return p->paused;
}

void au_html_parser_begin_script(AuHtmlParser *p)
{
    p->paused = false;
    p->tok.pause = false;
    save_ins(p);
}

void au_html_parser_end_script(AuHtmlParser *p)
{
    restore_ins(p);
}

bool au_html_parser_write(AuHtmlParser *p, const char *s, size_t len)
{
    HtmlTokenizer *t = &p->tok;
    if (t->ins == (size_t)-1)
        return false;
    p->tmp.len = 0;
    html_normalize_newlines(s, len, &p->tmp);
    size_t at = t->ins, n = p->tmp.len;
    html_tok_insert(t, at, p->tmp.data, n);
    t->ins += n;
    for (uint32_t i = 0; i < p->nins; i++)
        if (p->ins_stack[i] != (size_t)-1 && p->ins_stack[i] >= at)
            p->ins_stack[i] += n;
    if (!p->paused) {
        t->limit_ins++;
        run(p);
        t->limit_ins--;
    }
    return true;
}

AuDocument *au_html_parse(const char *bytes, size_t len, const AuHtmlOptions *opts)
{
    AuHtmlParser *p = au_html_parser_new(opts);
    au_html_parser_feed(p, bytes, len);
    au_html_parser_finish(p);
    AuDocument *doc = p->doc;
    au_node_ref(&doc->node);
    au_html_parser_free(p);
    return doc;
}

AuNode *au_html_parse_fragment(AuElement *context, const char *bytes, size_t len, const AuHtmlOptions *opts)
{
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    if (opts)
        o = *opts;
    AuDocument *tmpdoc = au_document_new();
    tmpdoc->quirks = context->node.doc ? context->node.doc->quirks : AU_QUIRKS_NONE;
    HtmlTokenizer tok;
    html_tok_init(&tok);
    HtmlTreeBuilder *tb = html_tb_new(tmpdoc, &tok, o.scripting);
    tok.emit = html_tb_emit;
    tok.cdata_ok = html_tb_cdata_ok;
    tok.sink = tb;
    tok.error = o.parse_error;
    tok.error_ctx = o.ctx;
    html_tb_set_fragment_context(tb, context);

    if (context->ns == AU_NS_HTML) {
        AuAtom t = context->tag;
        if (t == AU_ATOM_title || t == AU_ATOM_textarea)
            html_tok_set_state(&tok, HTML_STATE_RCDATA);
        else if (t == AU_ATOM_style || t == AU_ATOM_xmp || t == AU_ATOM_iframe || t == AU_ATOM_noembed ||
                 t == AU_ATOM_noframes || (t == AU_ATOM_noscript && o.scripting))
            html_tok_set_state(&tok, HTML_STATE_RAWTEXT);
        else if (t == AU_ATOM_script)
            html_tok_set_state(&tok, HTML_STATE_SCRIPT_DATA);
        else if (t == AU_ATOM_plaintext)
            html_tok_set_state(&tok, HTML_STATE_PLAINTEXT);
    }

    HtmlDecoder dec;
    html_decoder_init(&dec, AU_HTML_ENC_UTF8);
    dec.enc = AU_HTML_ENC_UTF8; /* a string: no BOM sniffing */
    AuBuf b;
    au_buf_init(&b);
    html_decode(&dec, bytes, len, true, &b);
    html_tok_append(&tok, b.data, b.len);
    au_buf_free(&b);
    tok.eof = true;
    html_tok_run(&tok);

    AuNode *frag = au_fragment_new(context->node.doc);
    au_node_reparent_children(html_tb_fragment_root(tb), frag);
    html_tb_free(tb);
    html_tok_free(&tok);
    au_node_unref(&tmpdoc->node);
    return frag;
}
