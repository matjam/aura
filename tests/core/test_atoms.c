/* Atoms and their scopes (src/core/core.c): an atom lives while a scope
 * or the program holds it; freeing a scope frees the atoms only it held
 * and their numbers are used again; sets carry a structure's atoms to a
 * later holder. */
#include "core/core.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            fails++;                                                                                                   \
            printf("%s:%d: ", __FILE__, __LINE__);                                                                     \
            printf(__VA_ARGS__);                                                                                       \
            putchar('\n');                                                                                             \
        }                                                                                                              \
    } while (0)

static uint32_t dynamic(void)
{
    AuAtomStats st;
    au_atom_stats(&st);
    return st.dynamic;
}

static AuAtom in_scope(AuAtomScope s, const char *str)
{
    AuAtomBits prev = au_atom_scope_enter(s);
    AuAtom a = au_atom_cstr(str);
    au_atom_scope_leave(prev);
    return a;
}

static void test_scopes(void)
{
    uint32_t base = dynamic();
    AuAtomScope s1 = au_atom_scope_new(), s2 = au_atom_scope_new();
    CHECK(s1 && s2 && s1 != s2, "two scopes: %u %u", s1, s2);
    AuAtom only1 = in_scope(s1, "only-in-one");
    AuAtom both = in_scope(s1, "in-both");
    CHECK(in_scope(s2, "in-both") == both, "the same string is the same atom in another scope");
    AuAtom only2 = in_scope(s2, "only-in-two");
    /* looked up in a scope: held by it too */
    AuAtomBits prev = au_atom_scope_enter(s2);
    CHECK(au_atom_lookup("only-in-one", 11) == only1, "lookup finds it");
    au_atom_scope_leave(prev);
    CHECK(dynamic() == base + 3, "%u dynamic atoms, want %u", dynamic(), base + 3);
    au_atom_scope_free(s1);
    CHECK(dynamic() == base + 3, "s2 holds all three (one through the lookup)");
    CHECK(au_atom_peek("only-in-one", 11) == only1, "kept by the lookup in s2");
    au_atom_scope_free(s2);
    CHECK(dynamic() == base, "%u dynamic atoms after both scopes, want %u", dynamic(), base);
    CHECK(!au_atom_peek("in-both", 7) && !au_atom_peek("only-in-two", 11), "freed");
    CHECK(!strcmp(au_atom_str(only2), "") && au_atom_len(only2) == 0, "a freed atom reads as empty");
    /* numbers are used again */
    AuAtomScope s3 = au_atom_scope_new();
    AuAtom again = in_scope(s3, "a-new-string");
    CHECK(again == only1 || again == both || again == only2, "a freed number is used again (%u)", again);
    CHECK(!strcmp(au_atom_str(again), "a-new-string"), "with its new string");
    au_atom_scope_free(s3);
    CHECK(dynamic() == base, "back to %u", base);
}

static void test_permanent(void)
{
    uint32_t base = dynamic();
    AuAtomScope s = au_atom_scope_new();
    AuAtom a = in_scope(s, "made-in-a-scope");
    /* touched outside any scope: lives for good */
    CHECK(au_atom_cstr("made-in-a-scope") == a, "same atom");
    AuAtom p = au_atom_permanent("permanent-one", 13);
    AuAtom q = in_scope(s, "permanent-two");
    CHECK(au_atom_permanent("permanent-two", 13) == q, "made permanent afterwards");
    au_atom_scope_free(s);
    CHECK(au_atom_peek("made-in-a-scope", 15) == a && au_atom_peek("permanent-one", 13) == p &&
              au_atom_peek("permanent-two", 13) == q,
          "all three stay");
    CHECK(dynamic() == base + 3, "%u, want %u", dynamic(), base + 3);
    /* compile-time atoms are never freed */
    s = au_atom_scope_new();
    CHECK(in_scope(s, "div") == AU_ATOM_div, "a static atom");
    au_atom_scope_free(s);
    CHECK(au_atom_peek("div", 3) == AU_ATOM_div, "still there");
    /* a freed scope that is still entered holds nothing: atoms made then
     * live for good rather than with a dead scope */
    s = au_atom_scope_new();
    AuAtomBits prev = au_atom_scope_enter(s);
    au_atom_scope_free(s);
    AuAtom late = au_atom_cstr("made-after-the-free");
    au_atom_scope_leave(prev);
    AuAtomScope reuse = au_atom_scope_new();
    au_atom_scope_free(reuse);
    CHECK(au_atom_peek("made-after-the-free", 19) == late, "kept although its scope's number came back");
}

static void test_heir(void)
{
    uint32_t base = dynamic();
    AuAtomScope top = au_atom_scope_new(), child = au_atom_scope_new();
    AuAtom a = in_scope(child, "made-by-the-frame");
    au_atom_scope_free_into(child, top);
    CHECK(au_atom_peek("made-by-the-frame", 17) == a, "the heir holds it");
    au_atom_scope_free(top);
    CHECK(dynamic() == base, "freed with the heir");
}

static void test_sets(void)
{
    uint32_t base = dynamic();
    AuAtomScope a = au_atom_scope_new(), b = au_atom_scope_new();
    AuAtomSet set = {0};
    AuAtomBits prev = au_atom_scope_enter(a);
    AuAtomCollect c = au_atom_collect_begin(&set, au_atom_scope_bits());
    for (int k = 0; k < 3; k++) /* repeats are kept once */
        for (int i = 0; i < 40; i++) {
            char name[32];
            snprintf(name, sizeof name, "set-name-%d", (i * 7) % 40);
            au_atom_cstr(name);
        }
    au_atom_cstr("span"); /* static: not in the set */
    au_atom_collect_end(c);
    AuAtom outside = au_atom_cstr("not-in-the-set");
    au_atom_scope_leave(prev);
    CHECK(set.n == 40 && set.sorted == 40, "%u atoms in the set, want 40", set.n);
    for (uint32_t i = 1; i < set.n; i++)
        CHECK(set.a[i - 1] < set.a[i], "sorted at %u", i);
    /* a second collection adds only what is new */
    prev = au_atom_scope_enter(a);
    c = au_atom_collect_begin(&set, au_atom_scope_bits());
    au_atom_cstr("set-name-3");
    au_atom_cstr("set-name-late");
    au_atom_collect_end(c);
    au_atom_scope_leave(prev);
    CHECK(set.n == 41, "%u after a late addition, want 41", set.n);
    /* b takes the set: a goes, the set's atoms stay, the other one goes */
    prev = au_atom_scope_enter(b);
    AuAtomBits bbits = au_atom_scope_bits();
    au_atom_scope_leave(prev);
    au_atom_set_hold(&set, bbits);
    au_atom_scope_free(a);
    CHECK(au_atom_peek("set-name-5", 10) && au_atom_peek("set-name-late", 13), "held by b");
    CHECK(!au_atom_peek("not-in-the-set", 14) && outside, "the atom outside the set went with a");
    CHECK(dynamic() == base + 41, "%u, want %u", dynamic(), base + 41);
    au_atom_scope_free(b);
    CHECK(dynamic() == base, "all freed with b");
    au_atom_set_free(&set);
}

static void test_limit(void)
{
    AuAtomScope s[40];
    int n = 0;
    for (; n < 40; n++)
        if (!(s[n] = au_atom_scope_new()))
            break;
    CHECK(n == 31, "%d scopes before none is left, want 31", n);
    /* with none left, atoms live for good */
    AuAtomBits prev = au_atom_scope_enter(0);
    AuAtom a = au_atom_cstr("no-scope-left");
    au_atom_scope_leave(prev);
    for (int i = 0; i < n; i++)
        au_atom_scope_free(s[i]);
    CHECK(au_atom_peek("no-scope-left", 13) == a, "kept");
}

static void test_many(void)
{
    /* the table shrinks and grows again across many pages' worth */
    uint32_t base = dynamic();
    for (int round = 0; round < 5; round++) {
        AuAtomScope s = au_atom_scope_new();
        AuAtomBits prev = au_atom_scope_enter(s);
        for (int i = 0; i < 20000; i++) {
            char name[32];
            snprintf(name, sizeof name, "r%d-class-%d", round, i);
            au_atom_cstr(name);
        }
        au_atom_scope_leave(prev);
        CHECK(dynamic() == base + 20000, "round %d: %u", round, dynamic());
        au_atom_scope_free(s);
        CHECK(dynamic() == base, "round %d freed: %u", round, dynamic());
    }
    AuAtomStats st;
    au_atom_stats(&st);
    CHECK(st.scopes == 0, "%u scopes live", st.scopes);
}

int main(void)
{
    test_scopes();
    test_permanent();
    test_heir();
    test_sets();
    test_limit();
    test_many();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
