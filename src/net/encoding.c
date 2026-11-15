/* Encoding labels (WHATWG Encoding Standard) and document encoding
 * detection: BOM, Content-Type charset, <meta> prescan, windows-1252. */
#include "policy.h"

#include <string.h>

/* Implemented in src/html/input.c (the HTML Standard's prescan). */
size_t au_html_prescan_charset(const uint8_t *data, size_t len, bool (*valid)(void *ctx, const char *label, size_t n),
                               void *ctx, char *out, size_t cap);

typedef struct Label {
    const char *label, *name;
} Label;

/* The complete label lists for UTF-8, windows-1252 (which ISO-8859-1 and
 * US-ASCII labels mean) and UTF-16, and each other encoding's name with
 * its common labels. Sorted by label for the binary search. */
static const Label labels[] = {
    {"866", "IBM866"},
    {"ansi_x3.4-1968", "windows-1252"},
    {"arabic", "ISO-8859-6"},
    {"ascii", "windows-1252"},
    {"big5", "Big5"},
    {"big5-hkscs", "Big5"},
    {"chinese", "GBK"},
    {"cp1250", "windows-1250"},
    {"cp1251", "windows-1251"},
    {"cp1252", "windows-1252"},
    {"cp1253", "windows-1253"},
    {"cp1254", "windows-1254"},
    {"cp1255", "windows-1255"},
    {"cp1256", "windows-1256"},
    {"cp1257", "windows-1257"},
    {"cp1258", "windows-1258"},
    {"cp819", "windows-1252"},
    {"cp866", "IBM866"},
    {"csbig5", "Big5"},
    {"cseuckr", "EUC-KR"},
    {"cseucpkdfmtjapanese", "EUC-JP"},
    {"csgb2312", "GBK"},
    {"csibm866", "IBM866"},
    {"csiso2022jp", "ISO-2022-JP"},
    {"csiso58gb231280", "GBK"},
    {"csiso88596e", "ISO-8859-6"},
    {"csiso88596i", "ISO-8859-6"},
    {"csiso88598e", "ISO-8859-8"},
    {"csiso88598i", "ISO-8859-8-I"},
    {"csisolatin1", "windows-1252"},
    {"csisolatin2", "ISO-8859-2"},
    {"csisolatin5", "windows-1254"},
    {"csisolatincyrillic", "ISO-8859-5"},
    {"csisolatingreek", "ISO-8859-7"},
    {"cskoi8r", "KOI8-R"},
    {"csmacintosh", "macintosh"},
    {"csshiftjis", "Shift_JIS"},
    {"csunicode", "UTF-16LE"},
    {"cyrillic", "ISO-8859-5"},
    {"euc-jp", "EUC-JP"},
    {"euc-kr", "EUC-KR"},
    {"gb18030", "gb18030"},
    {"gb2312", "GBK"},
    {"gb_2312", "GBK"},
    {"gb_2312-80", "GBK"},
    {"gbk", "GBK"},
    {"greek", "ISO-8859-7"},
    {"hebrew", "ISO-8859-8"},
    {"ibm819", "windows-1252"},
    {"ibm866", "IBM866"},
    {"iso-10646-ucs-2", "UTF-16LE"},
    {"iso-2022-jp", "ISO-2022-JP"},
    {"iso-8859-1", "windows-1252"},
    {"iso-8859-10", "ISO-8859-10"},
    {"iso-8859-11", "windows-874"},
    {"iso-8859-13", "ISO-8859-13"},
    {"iso-8859-14", "ISO-8859-14"},
    {"iso-8859-15", "ISO-8859-15"},
    {"iso-8859-16", "ISO-8859-16"},
    {"iso-8859-2", "ISO-8859-2"},
    {"iso-8859-3", "ISO-8859-3"},
    {"iso-8859-4", "ISO-8859-4"},
    {"iso-8859-5", "ISO-8859-5"},
    {"iso-8859-6", "ISO-8859-6"},
    {"iso-8859-7", "ISO-8859-7"},
    {"iso-8859-8", "ISO-8859-8"},
    {"iso-8859-8-i", "ISO-8859-8-I"},
    {"iso-8859-9", "windows-1254"},
    {"iso-ir-100", "windows-1252"},
    {"iso8859-1", "windows-1252"},
    {"iso8859-15", "ISO-8859-15"},
    {"iso8859-2", "ISO-8859-2"},
    {"iso88591", "windows-1252"},
    {"iso885915", "ISO-8859-15"},
    {"iso88592", "ISO-8859-2"},
    {"iso_8859-1", "windows-1252"},
    {"iso_8859-15", "ISO-8859-15"},
    {"iso_8859-1:1987", "windows-1252"},
    {"iso_8859-2", "ISO-8859-2"},
    {"koi", "KOI8-R"},
    {"koi8", "KOI8-R"},
    {"koi8-r", "KOI8-R"},
    {"koi8-ru", "KOI8-U"},
    {"koi8-u", "KOI8-U"},
    {"koi8_r", "KOI8-R"},
    {"ks_c_5601-1987", "EUC-KR"},
    {"l1", "windows-1252"},
    {"l2", "ISO-8859-2"},
    {"latin1", "windows-1252"},
    {"latin2", "ISO-8859-2"},
    {"mac", "macintosh"},
    {"macintosh", "macintosh"},
    {"ms932", "Shift_JIS"},
    {"ms_kanji", "Shift_JIS"},
    {"shift-jis", "Shift_JIS"},
    {"shift_jis", "Shift_JIS"},
    {"sjis", "Shift_JIS"},
    {"tis-620", "windows-874"},
    {"ucs-2", "UTF-16LE"},
    {"unicode", "UTF-16LE"},
    {"unicode-1-1-utf-8", "UTF-8"},
    {"unicode11utf8", "UTF-8"},
    {"unicode20utf8", "UTF-8"},
    {"unicodefeff", "UTF-16LE"},
    {"unicodefffe", "UTF-16BE"},
    {"us-ascii", "windows-1252"},
    {"utf-16", "UTF-16LE"},
    {"utf-16be", "UTF-16BE"},
    {"utf-16le", "UTF-16LE"},
    {"utf-8", "UTF-8"},
    {"utf8", "UTF-8"},
    {"windows-1250", "windows-1250"},
    {"windows-1251", "windows-1251"},
    {"windows-1252", "windows-1252"},
    {"windows-1253", "windows-1253"},
    {"windows-1254", "windows-1254"},
    {"windows-1255", "windows-1255"},
    {"windows-1256", "windows-1256"},
    {"windows-1257", "windows-1257"},
    {"windows-1258", "windows-1258"},
    {"windows-31j", "Shift_JIS"},
    {"windows-874", "windows-874"},
    {"x-cp1250", "windows-1250"},
    {"x-cp1251", "windows-1251"},
    {"x-cp1252", "windows-1252"},
    {"x-euc-jp", "EUC-JP"},
    {"x-gbk", "GBK"},
    {"x-mac-cyrillic", "x-mac-cyrillic"},
    {"x-mac-roman", "macintosh"},
    {"x-mac-ukrainian", "x-mac-cyrillic"},
    {"x-sjis", "Shift_JIS"},
    {"x-unicode20utf8", "UTF-8"},
    {"x-user-defined", "x-user-defined"},
};

const char *au_encoding_label(const char *s, size_t n)
{
    while (n && au_is_space((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n && au_is_space((unsigned char)s[n - 1]))
        n--;
    char low[32];
    if (n == 0 || n >= sizeof low)
        return NULL;
    for (size_t i = 0; i < n; i++)
        low[i] = (char)au_lower((unsigned char)s[i]);
    low[n] = 0;
    size_t lo = 0, hi = sizeof labels / sizeof labels[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(low, labels[mid].label);
        if (c == 0)
            return labels[mid].name;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static bool any_known(void *ctx, const char *label, size_t n)
{
    (void)ctx;
    return au_encoding_label(label, n) != NULL;
}

const char *au_detect_encoding(const uint8_t *d, size_t n, const char *charset)
{
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF)
        return "UTF-8";
    if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF)
        return "UTF-16BE";
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE)
        return "UTF-16LE";
    if (charset && charset[0]) {
        const char *e = au_encoding_label(charset, strlen(charset));
        if (e && strcmp(e, "replacement"))
            return e;
    }
    char label[64];
    size_t ll = au_html_prescan_charset(d, n, any_known, NULL, label, sizeof label);
    if (ll) {
        const char *e = au_encoding_label(label, ll);
        if (e) {
            /* A meta naming UTF-16 means UTF-8; x-user-defined means
             * windows-1252 (HTML 13.2.3.2). */
            if (!strcmp(e, "UTF-16LE") || !strcmp(e, "UTF-16BE"))
                return "UTF-8";
            if (!strcmp(e, "x-user-defined"))
                return "windows-1252";
            return e;
        }
    }
    return "windows-1252";
}
