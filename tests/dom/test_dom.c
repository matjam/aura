/* Tests for src/core and src/dom. Exits non-zero on the first failure. */
#include "dom/dom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

static void test_atoms(void)
{
    CHECK(au_atom_cstr("id") == AU_ATOM_id);
    CHECK(au_atom_cstr("body") == AU_ATOM_body);
    AuAtom a = au_atom("frobnicate", 10);
    CHECK(a >= AU_ATOM_STATIC_COUNT);
    CHECK(au_atom_cstr("frobnicate") == a);
    CHECK(strcmp(au_atom_str(a), "frobnicate") == 0);
    CHECK(au_atom_lookup("never-interned", 14) == 0);
    CHECK(au_atom("", 0) == 0);
    /* Enough atoms to grow the table several times. */
    char name[32];
    AuAtom first = 0;
    for (int i = 0; i < 5000; i++) {
        int n = snprintf(name, sizeof name, "atom-%d", i);
        AuAtom x = au_atom(name, (size_t)n);
        if (!i)
            first = x;
    }
    CHECK(au_atom_cstr("atom-0") == first);
    CHECK(au_atom_cstr("atom-4999") == first + 4999);
    CHECK(au_atom_cstr("frobnicate") == a);
}

static void test_utf8(void)
{
    size_t adv;
    CHECK(au_utf8_decode("A", 1, &adv) == 'A' && adv == 1);
    CHECK(au_utf8_decode("\xC3\xA9", 2, &adv) == 0xE9 && adv == 2);
    CHECK(au_utf8_decode("\xE2\x82\xAC", 3, &adv) == 0x20AC && adv == 3);
    CHECK(au_utf8_decode("\xF0\x9F\x98\x80", 4, &adv) == 0x1F600 && adv == 4);
    CHECK(au_utf8_decode("\xC0\x80", 2, &adv) == 0xFFFD && adv == 1); /* overlong */
    CHECK(au_utf8_decode("\xE2\x82", 2, &adv) == 0xFFFD && adv == 1); /* truncated */
    CHECK(au_utf8_decode("\xED\xA0\x80", 3, &adv) == 0xFFFD);         /* surrogate */
    AuBuf b;
    au_buf_init(&b);
    au_buf_put_utf8(&b, 0x1F600);
    au_buf_printf(&b, "%d-%s", 42, "x");
    CHECK(strcmp(au_buf_cstr(&b), "\xF0\x9F\x98\x80" "42-x") == 0);
    au_buf_free(&b);
}

static void test_tree(void)
{
    AuDocument *doc = au_document_new();
    AuNode *html = au_element_new(doc, AU_ATOM_html, AU_NS_HTML);
    AuNode *body = au_element_new(doc, AU_ATOM_body, AU_NS_HTML);
    AuNode *head = au_element_new(doc, AU_ATOM_head, AU_NS_HTML);
    au_node_append(&doc->node, html);
    au_node_append(html, body);
    au_node_insert_before(html, head, body);
    au_node_unref(html);
    au_node_unref(head);
    CHECK(html->first_child == head && head->next == body && html->last_child == body);
    CHECK(au_document_head(doc) == (AuElement *)head);
    CHECK(au_document_body(doc) == (AuElement *)body);

    AuNode *t = au_text_new(doc, "hello", 5);
    au_node_append(body, t);
    au_node_unref(t);
    au_text_append(au_as_text(t), ", world", 7);
    CHECK(strcmp(au_as_text(t)->data, "hello, world") == 0);

    /* Preorder: html head body text. */
    AuNode *n = &doc->node;
    n = au_node_next(n, &doc->node); CHECK(n == html);
    n = au_node_next(n, &doc->node); CHECK(n == head);
    n = au_node_next(n, &doc->node); CHECK(n == body);
    n = au_node_next(n, &doc->node); CHECK(n == t);
    n = au_node_next(n, &doc->node); CHECK(n == NULL);

    /* Moving a node between parents keeps it alive. */
    au_node_append(head, t);
    CHECK(body->first_child == NULL && head->first_child == t && t->parent == head);

    /* A referenced node survives its tree; body still holds one ref. */
    au_node_unref(&doc->node);
    CHECK(body->parent == NULL && body->refs == 1);
    au_node_unref(body);
}

static void test_attrs(void)
{
    AuDocument *doc = au_document_new();
    AuElement *el = au_as_element(au_element_new(doc, AU_ATOM_body, AU_NS_HTML));
    AuAtom href = au_atom_cstr("href");
    CHECK(au_element_get_attr(el, href) == NULL);
    au_element_set_attr(el, href, "a", 1);
    au_element_set_attr(el, href, "/b", 2);
    CHECK(el->nattrs == 1 && strcmp(au_element_get_attr(el, href), "/b") == 0);
    au_element_set_attr(el, AU_ATOM_id, "main", 4);
    au_element_set_attr(el, AU_ATOM_class, "  one two\tone ", 14);
    CHECK(el->id == au_atom_cstr("main"));
    CHECK(el->nclasses == 2);
    CHECK(au_element_has_class(el, au_atom_cstr("one")) && au_element_has_class(el, au_atom_cstr("two")));
    au_element_remove_attr(el, AU_ATOM_class);
    CHECK(el->nclasses == 0 && el->nattrs == 2);
    au_element_remove_attr(el, AU_ATOM_id);
    CHECK(el->id == 0);
    au_node_unref(&el->node);
    au_node_unref(&doc->node);
}

/* A chain deep enough to overflow the stack if destruction recursed. */
static void test_deep(void)
{
    AuDocument *doc = au_document_new();
    AuNode *parent = &doc->node;
    for (int i = 0; i < 200000; i++) {
        AuNode *el = au_element_new(doc, AU_ATOM_body, AU_NS_HTML);
        au_node_append(parent, el);
        au_node_unref(el);
        parent = el;
    }
    au_node_unref(&doc->node);
}

int main(void)
{
    test_atoms();
    test_utf8();
    test_tree();
    test_attrs();
    test_deep();
    printf("dom: ok\n");
    return 0;
}
