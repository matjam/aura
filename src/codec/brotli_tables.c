/* Aura brotli decoder: constant tables from RFC 7932 (context lookup
 * tables, section 7.1; word transforms, section 8 and Appendix B). */
#include "brotli_int.h"

#include <string.h>

/* UTF8 context mode, first table: the class of the last byte. */
const uint8_t au_brotli_lut0[256] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  4,  0,  0,  4,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    8,  12, 16, 12, 12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12,
    44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12,
    12, 48, 52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48,
    52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12, 12,
    12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56,
    60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28, 12, 0,
    0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,
    0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,
    0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,
    0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,
    2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,
    2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,
    2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,
    2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,
};

/* UTF8 context mode, second table: the class of the byte before. */
const uint8_t au_brotli_lut1[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};

/* Signed context mode: both bytes use this table. */
const uint8_t au_brotli_lut2[256] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7,
};

/* Elementary transforms. T_OMIT_LAST1 + n - 1 omits the last n bytes;
 * T_OMIT_FIRST1 + n - 1 omits the first n. */
enum { T_ID = 0, T_OMIT_LAST1 = 1, T_UPPER_FIRST = 10, T_UPPER_ALL = 11, T_OMIT_FIRST1 = 12 };
#define OL(n) (T_OMIT_LAST1 + (n)-1)
#define OF(n) (T_OMIT_FIRST1 + (n)-1)
#define UF T_UPPER_FIRST
#define UA T_UPPER_ALL

static const struct {
    const char *prefix;
    uint8_t type;
    const char *suffix;
} transforms[AU_BROTLI_NUM_TRANSFORMS] = {
    {"", T_ID, ""},           {"", T_ID, " "},           {" ", T_ID, " "},         {"", OF(1), ""},
    {"", UF, " "},            {"", T_ID, " the "},       {" ", T_ID, ""},          {"s ", T_ID, " "},
    {"", T_ID, " of "},       {"", UF, ""},              {"", T_ID, " and "},      {"", OF(2), ""},
    {"", OL(1), ""},          {", ", T_ID, " "},         {"", T_ID, ", "},         {" ", UF, " "},
    {"", T_ID, " in "},       {"", T_ID, " to "},        {"e ", T_ID, " "},        {"", T_ID, "\""},
    {"", T_ID, "."},          {"", T_ID, "\">"},         {"", T_ID, "\n"},         {"", OL(3), ""},
    {"", T_ID, "]"},          {"", T_ID, " for "},       {"", OF(3), ""},          {"", OL(2), ""},
    {"", T_ID, " a "},        {"", T_ID, " that "},      {" ", UF, ""},            {"", T_ID, ". "},
    {".", T_ID, ""},          {" ", T_ID, ", "},         {"", OF(4), ""},          {"", T_ID, " with "},
    {"", T_ID, "'"},          {"", T_ID, " from "},      {"", T_ID, " by "},       {"", OF(5), ""},
    {"", OF(6), ""},          {" the ", T_ID, ""},       {"", OL(4), ""},          {"", T_ID, ". The "},
    {"", UA, ""},             {"", T_ID, " on "},        {"", T_ID, " as "},       {"", T_ID, " is "},
    {"", OL(7), ""},          {"", OL(1), "ing "},       {"", T_ID, "\n\t"},       {"", T_ID, ":"},
    {" ", T_ID, ". "},        {"", T_ID, "ed "},         {"", OF(9), ""},          {"", OF(7), ""},
    {"", OL(6), ""},          {"", T_ID, "("},           {"", UF, ", "},           {"", OL(8), ""},
    {"", T_ID, " at "},       {"", T_ID, "ly "},         {" the ", T_ID, " of "},  {"", OL(5), ""},
    {"", OL(9), ""},          {" ", UF, ", "},           {"", UF, "\""},           {".", T_ID, "("},
    {"", UA, " "},            {"", UF, "\">"},           {"", T_ID, "=\""},        {" ", T_ID, "."},
    {".com/", T_ID, ""},      {" the ", T_ID, " of the "}, {"", UF, "'"},          {"", T_ID, ". This "},
    {"", T_ID, ","},          {".", T_ID, " "},          {"", UF, "("},            {"", UF, "."},
    {"", T_ID, " not "},      {" ", T_ID, "=\""},        {"", T_ID, "er "},        {" ", UA, " "},
    {"", T_ID, "al "},        {" ", UA, ""},             {"", T_ID, "='"},         {"", UA, "\""},
    {"", UF, ". "},           {" ", T_ID, "("},          {"", T_ID, "ful "},       {" ", UF, ". "},
    {"", T_ID, "ive "},       {"", T_ID, "less "},       {"", UA, "'"},            {"", T_ID, "est "},
    {" ", UF, "."},           {"", UA, "\">"},           {" ", T_ID, "='"},        {"", UF, ","},
    {"", T_ID, "ize "},       {"", UA, "."},             {"\xc2\xa0", T_ID, ""},   {" ", T_ID, ","},
    {"", UF, "=\""},          {"", UA, "=\""},           {"", T_ID, "ous "},       {"", UA, ", "},
    {"", UF, "='"},           {" ", UF, ","},            {" ", UA, "=\""},         {" ", UA, ", "},
    {"", UA, ","},            {"", UA, "("},             {"", UA, ". "},           {" ", UA, "."},
    {"", UA, "='"},           {" ", UA, ". "},           {" ", UF, "=\""},         {" ", UA, "='"},
    {" ", UF, "='"},
};

/* RFC 7932 section 8: uppercases the character at p, returns how many
 * bytes it spans. May touch p[1] and p[2]; the caller leaves room. */
static uint32_t to_upper(uint8_t *p)
{
    if (p[0] < 0xc0) {
        if (p[0] >= 'a' && p[0] <= 'z')
            p[0] ^= 32;
        return 1;
    }
    if (p[0] < 0xe0) {
        p[1] ^= 32;
        return 2;
    }
    p[2] ^= 5;
    return 3;
}

uint32_t au_brotli_transform(uint8_t *dst, const uint8_t *word, uint32_t len, uint32_t id)
{
    const char *pre = transforms[id].prefix, *suf = transforms[id].suffix;
    uint32_t type = transforms[id].type;
    size_t plen = strlen(pre), slen = strlen(suf);
    uint32_t n = 0, skip = 0, i;

    memcpy(dst, pre, plen);
    n = (uint32_t)plen;
    if (type >= T_OMIT_FIRST1) {
        skip = type - T_OMIT_FIRST1 + 1;
        if (skip > len)
            skip = len;
        word += skip;
        len -= skip;
    } else if (type >= T_OMIT_LAST1 && type < T_UPPER_FIRST) {
        uint32_t cut = type - T_OMIT_LAST1 + 1;
        len = cut > len ? 0 : len - cut;
    }
    memcpy(dst + n, word, len);
    /* The uppercase step may touch two bytes past the word; clear them so
     * the result never depends on stale buffer contents. The suffix is
     * copied over them afterwards. */
    dst[n + len] = 0;
    dst[n + len + 1] = 0;
    if (type == T_UPPER_FIRST && len > 0) {
        to_upper(dst + n);
    } else if (type == T_UPPER_ALL) {
        for (i = 0; i < len;)
            i += to_upper(dst + n + i);
    }
    n += len;
    memcpy(dst + n, suf, slen);
    return n + (uint32_t)slen;
}
