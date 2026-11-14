/* Runs the html5lib tokenizer tests (*.test JSON files).
 *
 * Usage: test_tokenizer [-v] FILE.test...
 *
 * Each test runs once per initial state. The input goes through the
 * input decoder (newline normalization) and is fed both as one chunk and
 * one byte at a time; both runs must produce the expected tokens and the
 * expected parse error codes, in order, at the expected positions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html/html_internal.h"
#include "json.h"

static int verbose;

typedef struct {
    AuBuf out;      /* canonical token list */
    AuBuf chars;    /* pending coalesced characters */
    AuBuf errs;     /* error codes */
    AuBuf errpos;   /* error codes with positions */
} Rec;

static void put_str(AuBuf *b, const char *s, size_t n)
{
    au_buf_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            au_buf_putc(b, '\\');
            au_buf_putc(b, (char)c);
        } else if (c < 0x20) {
            au_buf_printf(b, "\\x%02x", c);
        } else {
            au_buf_putc(b, (char)c);
        }
    }
    au_buf_putc(b, '"');
}

static void flush(Rec *r)
{
    if (!r->chars.len)
        return;
    au_buf_puts(&r->out, "Character ");
    put_str(&r->out, r->chars.data, r->chars.len);
    au_buf_putc(&r->out, '\n');
    r->chars.len = 0;
}

typedef struct {
    const char *name;
    const char *val;
    size_t vlen;
} KV;

static int kv_cmp(const void *a, const void *b)
{
    return strcmp(((const KV *)a)->name, ((const KV *)b)->name);
}

static void put_attrs(AuBuf *b, KV *kv, size_t n)
{
    qsort(kv, n, sizeof *kv, kv_cmp);
    au_buf_putc(b, '{');
    for (size_t i = 0; i < n; i++) {
        put_str(b, kv[i].name, strlen(kv[i].name));
        au_buf_putc(b, '=');
        put_str(b, kv[i].val, kv[i].vlen);
        au_buf_putc(b, ',');
    }
    au_buf_putc(b, '}');
}

static void sink(void *ctx, HtmlToken *t)
{
    Rec *r = ctx;
    if (t->type == HTML_TOK_CHARS) {
        au_buf_append(&r->chars, t->data, t->len);
        return;
    }
    flush(r);
    AuBuf *o = &r->out;
    switch (t->type) {
    case HTML_TOK_DOCTYPE:
        au_buf_puts(o, "DOCTYPE ");
        if (t->has_name)
            put_str(o, t->data, t->len);
        else
            au_buf_puts(o, "null");
        au_buf_putc(o, ' ');
        if (t->has_public)
            put_str(o, t->pub, t->publen);
        else
            au_buf_puts(o, "null");
        au_buf_putc(o, ' ');
        if (t->has_system)
            put_str(o, t->sys, t->syslen);
        else
            au_buf_puts(o, "null");
        au_buf_puts(o, t->force_quirks ? " false\n" : " true\n");
        break;
    case HTML_TOK_START: {
        au_buf_puts(o, "StartTag ");
        put_str(o, au_atom_str(t->name), au_atom_len(t->name));
        au_buf_putc(o, ' ');
        KV *kv = malloc((t->nattrs + 1) * sizeof *kv);
        for (uint32_t i = 0; i < t->nattrs; i++) {
            kv[i].name = au_atom_str(t->attrs[i].name);
            kv[i].val = t->vals ? t->vals + t->attrs[i].voff : "";
            kv[i].vlen = t->attrs[i].vlen;
        }
        put_attrs(o, kv, t->nattrs);
        free(kv);
        au_buf_puts(o, t->self_closing ? " true\n" : "\n");
        break;
    }
    case HTML_TOK_END:
        au_buf_puts(o, "EndTag ");
        put_str(o, au_atom_str(t->name), au_atom_len(t->name));
        au_buf_putc(o, '\n');
        break;
    case HTML_TOK_COMMENT:
        au_buf_puts(o, "Comment ");
        put_str(o, t->data, t->len);
        au_buf_putc(o, '\n');
        break;
    case HTML_TOK_PI:
        au_buf_puts(o, "ProcessingInstruction ");
        put_str(o, au_atom_str(t->name), au_atom_len(t->name));
        au_buf_putc(o, ' ');
        put_str(o, t->data, t->len);
        au_buf_putc(o, '\n');
        break;
    default:
        break;
    }
}

static void on_error(void *ctx, const char *code, int line, int col)
{
    Rec *r = ctx;
    au_buf_printf(&r->errs, "%s\n", code);
    au_buf_printf(&r->errpos, "%s %d:%d\n", code, line, col);
}

/* Renders the expected output of a test in the same form as sink. */
static void expected(const JVal *output, int dbl, AuBuf *o)
{
    AuBuf chars;
    au_buf_init(&chars);
#define S(v, b) put_jstr(v, dbl, b)
    for (size_t i = 0; output && i < output->n; i++) {
        const JVal *tk = output->items[i];
        const char *kind = tk->items[0]->str;
        char *tmp;
        size_t tlen;
        if (!strcmp(kind, "Character")) {
            if (dbl) {
                tmp = json_unescape_u(tk->items[1]->str, tk->items[1]->len, &tlen);
                au_buf_append(&chars, tmp, tlen);
                free(tmp);
            } else {
                au_buf_append(&chars, tk->items[1]->str, tk->items[1]->len);
            }
            continue;
        }
        if (chars.len) {
            au_buf_puts(o, "Character ");
            put_str(o, chars.data, chars.len);
            au_buf_putc(o, '\n');
            chars.len = 0;
        }
        au_buf_puts(o, kind);
        for (size_t j = 1; j < tk->n; j++) {
            const JVal *v = tk->items[j];
            au_buf_putc(o, ' ');
            if (v->type == J_NULL) {
                au_buf_puts(o, "null");
            } else if (v->type == J_TRUE) {
                au_buf_puts(o, "true");
            } else if (v->type == J_FALSE) {
                au_buf_puts(o, "false");
            } else if (v->type == J_STR) {
                if (dbl) {
                    tmp = json_unescape_u(v->str, v->len, &tlen);
                    put_str(o, tmp, tlen);
                    free(tmp);
                } else {
                    put_str(o, v->str, v->len);
                }
            } else if (v->type == J_OBJ) {
                KV *kv = malloc((v->n + 1) * sizeof *kv);
                char **owned = calloc(v->n + 1, sizeof *owned);
                for (size_t k = 0; k < v->n; k++) {
                    kv[k].name = v->keys[k];
                    if (dbl) {
                        owned[k] = json_unescape_u(v->items[k]->str, v->items[k]->len, &tlen);
                        kv[k].val = owned[k];
                        kv[k].vlen = tlen;
                    } else {
                        kv[k].val = v->items[k]->str;
                        kv[k].vlen = v->items[k]->len;
                    }
                }
                put_attrs(o, kv, v->n);
                for (size_t k = 0; k < v->n; k++)
                    free(owned[k]);
                free(owned);
                free(kv);
            }
        }
        au_buf_putc(o, '\n');
    }
    if (chars.len) {
        au_buf_puts(o, "Character ");
        put_str(o, chars.data, chars.len);
        au_buf_putc(o, '\n');
    }
    au_buf_free(&chars);
#undef S
}

static int state_of(const char *s)
{
    if (!strcmp(s, "Data state"))
        return HTML_STATE_DATA;
    if (!strcmp(s, "PLAINTEXT state"))
        return HTML_STATE_PLAINTEXT;
    if (!strcmp(s, "RCDATA state"))
        return HTML_STATE_RCDATA;
    if (!strcmp(s, "RAWTEXT state"))
        return HTML_STATE_RAWTEXT;
    if (!strcmp(s, "Script data state"))
        return HTML_STATE_SCRIPT_DATA;
    if (!strcmp(s, "CDATA section state"))
        return HTML_STATE_CDATA_SECTION;
    return -1;
}

static void run_one(const char *input, size_t len, int state, const char *last, int bytewise, Rec *r)
{
    HtmlTokenizer t;
    HtmlDecoder d;
    AuBuf dec;
    au_buf_init(&dec);
    html_tok_init(&t);
    html_decoder_init(&d, AU_HTML_ENC_UTF8);
    /* The tokenizer tests have no BOM handling: decode as plain UTF-8. */
    d.enc = AU_HTML_ENC_UTF8;
    t.emit = sink;
    t.sink = r;
    t.error = on_error;
    t.error_ctx = r;
    html_tok_set_state(&t, state);
    if (last)
        html_tok_set_last_start_tag(&t, au_atom_cstr(last));
    size_t step = bytewise ? 1 : (len ? len : 1);
    for (size_t i = 0; i < len; i += step) {
        size_t n = len - i < step ? len - i : step;
        dec.len = 0;
        html_decode(&d, input + i, n, false, &dec);
        html_tok_append(&t, dec.data, dec.len);
        html_tok_run(&t);
    }
    dec.len = 0;
    html_decode(&d, "", 0, true, &dec);
    html_tok_append(&t, dec.data, dec.len);
    t.eof = true;
    html_tok_run(&t);
    flush(r);
    html_tok_free(&t);
    au_buf_free(&dec);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    int total_pass = 0, total_fail = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-v")) {
            verbose = 1;
            continue;
        }
        size_t flen;
        char *src = read_file(argv[a], &flen);
        if (!src) {
            fprintf(stderr, "cannot read %s\n", argv[a]);
            return 1;
        }
        JVal *root = json_parse(src, flen);
        JVal *tests = json_get(root, "tests");
        if (!tests)
            tests = json_get(root, "xmlViolationTests");
        if (!tests) {
            fprintf(stderr, "%s: no tests\n", argv[a]);
            return 1;
        }
        int pass = 0, fail = 0;
        for (size_t i = 0; i < tests->n; i++) {
            JVal *tc = tests->items[i];
            if (json_get(root, "xmlViolationTests")) {
                /* These expect the infoset coercion of 13.2.9, which is
                 * not part of the parser. */
                continue;
            }
            JVal *in = json_get(tc, "input");
            JVal *dbl = json_get(tc, "doubleEscaped");
            int isdbl = dbl && dbl->type == J_TRUE;
            char *input;
            size_t ilen;
            if (isdbl) {
                input = json_unescape_u(in->str, in->len, &ilen);
            } else {
                input = malloc(in->len + 1);
                memcpy(input, in->str, in->len + 1);
                ilen = in->len;
            }
            AuBuf want, wanterr;
            au_buf_init(&want);
            au_buf_init(&wanterr);
            expected(json_get(tc, "output"), isdbl, &want);
            JVal *errs = json_get(tc, "errors");
            for (size_t e = 0; errs && e < errs->n; e++) {
                JVal *code = json_get(errs->items[e], "code");
                JVal *line = json_get(errs->items[e], "line");
                JVal *col = json_get(errs->items[e], "col");
                au_buf_printf(&wanterr, "%s %d:%d\n", code->str, line ? (int)line->num : 0,
                              col ? (int)col->num : 0);
            }
            JVal *states = json_get(tc, "initialStates");
            JVal *last = json_get(tc, "lastStartTag");
            size_t ns = states ? states->n : 1;
            int ok = 1;
            for (size_t s = 0; s < ns; s++) {
                int st = states ? state_of(states->items[s]->str) : HTML_STATE_DATA;
                for (int bytewise = 0; bytewise < 2; bytewise++) {
                    Rec r;
                    memset(&r, 0, sizeof r);
                    run_one(input, ilen, st, last ? last->str : NULL, bytewise, &r);
                    const char *got = r.out.data ? au_buf_cstr(&r.out) : "";
                    const char *goterr = r.errpos.data ? au_buf_cstr(&r.errpos) : "";
                    const char *w = want.data ? au_buf_cstr(&want) : "";
                    const char *we = wanterr.data ? au_buf_cstr(&wanterr) : "";
                    if (strcmp(got, w) || strcmp(goterr, we)) {
                        if (ok && verbose) {
                            JVal *desc = json_get(tc, "description");
                            printf("FAIL %s: %s [state %d%s]\n--- input\n%s\n--- want\n%s%s--- got\n%s%s\n",
                                   argv[a], desc ? desc->str : "", st, bytewise ? ", bytewise" : "",
                                   input, w, we, got, goterr);
                        }
                        ok = 0;
                    }
                    au_buf_free(&r.out);
                    au_buf_free(&r.chars);
                    au_buf_free(&r.errs);
                    au_buf_free(&r.errpos);
                }
            }
            if (ok)
                pass++;
            else
                fail++;
            au_buf_free(&want);
            au_buf_free(&wanterr);
            free(input);
        }
        const char *base = strrchr(argv[a], '/');
        printf("%-32s %5d passed %5d failed\n", base ? base + 1 : argv[a], pass, fail);
        total_pass += pass;
        total_fail += fail;
        json_free(root);
        free(src);
    }
    printf("tokenizer total: %d passed, %d failed\n", total_pass, total_fail);
    return total_fail ? 1 : 0;
}
