/* The HTML fragment serialization algorithm (HTML 13.3), without
 * recursion: open elements are kept on an explicit stack. */
#include "html_internal.h"

static void escape(AuBuf *out, const char *s, size_t n, bool attr)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        size_t skip = 1;
        unsigned char c = (unsigned char)s[i];
        if (c == '&')
            rep = "&amp;";
        else if (c == '<')
            rep = "&lt;";
        else if (c == '>')
            rep = "&gt;";
        else if (c == '"' && attr)
            rep = "&quot;";
        else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) {
            rep = "&nbsp;";
            skip = 2;
        }
        if (rep) {
            au_buf_append(out, s + start, i - start);
            au_buf_puts(out, rep);
            i += skip - 1;
            start = i + 1;
        }
    }
    au_buf_append(out, s + start, n - start);
}

static bool is_void(const AuNode *n)
{
    const AuElement *e = (const AuElement *)n;
    return n->type == AU_NODE_ELEMENT && e->ns == AU_NS_HTML && html_serializes_void(e->tag);
}

/* The node whose children are serialized for n. */
static const AuNode *container(const AuNode *n)
{
    const AuElement *e = (const AuElement *)n;
    if (n->type == AU_NODE_ELEMENT && e->ns == AU_NS_HTML && e->tag == AU_ATOM_template && e->template_content)
        return e->template_content;
    return n;
}

static void open_node(const AuNode *n, AuBuf *out, bool scripting)
{
    switch (n->type) {
    case AU_NODE_ELEMENT: {
        const AuElement *e = (const AuElement *)n;
        au_buf_putc(out, '<');
        au_buf_append(out, au_atom_str(e->tag), au_atom_len(e->tag));
        for (uint32_t i = 0; i < e->nattrs; i++) {
            au_buf_putc(out, ' ');
            au_buf_append(out, au_atom_str(e->attrs[i].name), au_atom_len(e->attrs[i].name));
            au_buf_puts(out, "=\"");
            escape(out, e->attrs[i].value, e->attrs[i].len, true);
            au_buf_putc(out, '"');
        }
        au_buf_putc(out, '>');
        break;
    }
    case AU_NODE_TEXT: {
        const AuText *t = (const AuText *)n;
        const AuNode *p = n->parent;
        bool literal = false;
        if (p && p->type == AU_NODE_ELEMENT) {
            const AuElement *pe = (const AuElement *)p;
            if (pe->ns == AU_NS_HTML &&
                (html_serializes_rawtext(pe->tag) || (pe->tag == AU_ATOM_noscript && scripting)))
                literal = true;
        }
        if (literal)
            au_buf_append(out, t->data, t->len);
        else
            escape(out, t->data, t->len, false);
        break;
    }
    case AU_NODE_COMMENT: {
        const AuText *t = (const AuText *)n;
        au_buf_puts(out, "<!--");
        au_buf_append(out, t->data, t->len);
        au_buf_puts(out, "-->");
        break;
    }
    case AU_NODE_PROCESSING_INSTRUCTION: {
        const AuProcessingInstruction *pi = (const AuProcessingInstruction *)n;
        au_buf_puts(out, "<?");
        au_buf_append(out, au_atom_str(pi->target), au_atom_len(pi->target));
        au_buf_putc(out, ' ');
        au_buf_append(out, pi->text.data, pi->text.len);
        au_buf_puts(out, "?>");
        break;
    }
    case AU_NODE_DOCTYPE:
        au_buf_puts(out, "<!DOCTYPE ");
        au_buf_puts(out, ((const AuDoctype *)n)->name);
        au_buf_putc(out, '>');
        break;
    default:
        break;
    }
}

static void close_element(const AuNode *n, AuBuf *out)
{
    const AuElement *e = (const AuElement *)n;
    au_buf_puts(out, "</");
    au_buf_append(out, au_atom_str(e->tag), au_atom_len(e->tag));
    au_buf_putc(out, '>');
}

void au_html_serialize(const AuNode *node, AuBuf *out, bool scripting)
{
    if (is_void(node))
        return;
    const AuNode **stack = NULL;
    size_t depth = 0, cap = 0;
    const AuNode *n = container(node)->first_child;
    for (;;) {
        if (!n) {
            if (!depth)
                break;
            n = stack[--depth];
            close_element(n, out);
            n = n->next;
            continue;
        }
        open_node(n, out, scripting);
        if (n->type == AU_NODE_ELEMENT && !is_void(n)) {
            const AuNode *c = container(n)->first_child;
            if (c) {
                if (depth == cap) {
                    cap = cap ? cap * 2 : 32;
                    stack = au_realloc(stack, cap * sizeof *stack);
                }
                stack[depth++] = n;
                n = c;
                continue;
            }
            close_element(n, out);
        }
        n = n->next;
    }
    au_free(stack);
}
