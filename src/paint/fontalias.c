/* The font alias table: what to draw when a page asks for a family that
 * is not installed.
 *
 * Each row names a family as pages spell it and lists, in order, the
 * families to try in its place. Installed system families come first in a
 * row, and the faces bundled with Aura (Aura Sans, Aura Serif and Aura
 * Mono, built from Liberation; Inter; DejaVu Sans; Noto Emoji) come last,
 * so a machine that has the real font, or a close system one, uses it.
 *
 * Rows for the CSS generic families also say which preference slot
 * (AuGenericFamily) supplies the user's choice; that choice is tried
 * before the row's targets. A target may name a generic family, which
 * expands to that generic's preference and row (one level deep).
 *
 * To add an alias, add a row: the name as pages write it (matching is
 * ASCII case-insensitive) and its targets, best first. The text and emoji
 * fallback lists at the end are the faces tried for a character that no
 * family of the page's list has. */
#include "font.h"
#include "paint_internal.h"

#include <string.h>

#define SANS_MODERN "Inter, Aura Sans"
#define MONO_MODERN "Menlo, Monaco, DejaVu Sans Mono, Andale Mono, Courier New, Aura Mono"
/* Colour emoji first; the monochrome face covers text presentation
 * (U+FE0E) and what the colour face lacks. */
#define EMOJI "Twemoji Mozilla, Noto Emoji"

/* The CSS generic families. */
static const AuFontAlias generics[] = {
    {"serif", AU_GENERIC_SERIF, "Times New Roman, Times, Georgia, Aura Serif, Liberation Serif, DejaVu Serif"},
    {"sans-serif", AU_GENERIC_SANS_SERIF, "Helvetica, Arial, Geneva, Aura Sans, Liberation Sans, DejaVu Sans"},
    {"monospace", AU_GENERIC_MONOSPACE, "Courier New, Courier, Monaco, Aura Mono, Liberation Mono, DejaVu Sans Mono"},
    {"system-ui", AU_GENERIC_SYSTEM_UI, "Charcoal, Geneva, Lucida Grande, sans-serif"},
    {"cursive", AU_GENERIC_CURSIVE, "Apple Chancery, Comic Sans MS, Zapf Chancery, serif"},
    {"fantasy", AU_GENERIC_FANTASY, "Papyrus, Impact, Comic Sans MS, serif"},
    {"ui-serif", AU_GENERIC_SERIF, "New York, serif"},
    {"ui-sans-serif", AU_GENERIC_SANS_SERIF, "sans-serif"},
    {"ui-monospace", AU_GENERIC_MONOSPACE, "SF Mono, Menlo, Monaco, monospace"},
    {"ui-rounded", -1, "SF Pro Rounded, Arial Rounded MT Bold, VAG Rounded, Inter, sans-serif"},
    {"math", -1, "Cambria Math, STIX Two Math, STIXGeneral, DejaVu Sans, serif"},
    {"emoji", -1, "Apple Color Emoji, Segoe UI Emoji, Noto Color Emoji, " EMOJI},
    {"fangsong", -1, "serif"},
};

/* Named families. */
static const AuFontAlias rows[] = {
    /* Sans-serif. */
    {"Arial", -1, "Helvetica, Liberation Sans, Arimo, Aura Sans"},
    {"Helvetica", -1, "Arial, Liberation Sans, Arimo, Aura Sans"},
    {"Helvetica Neue", -1, "Helvetica, Arial, Aura Sans"},
    {"Liberation Sans", -1, "Arimo, Arial, Helvetica, Aura Sans"},
    {"Arimo", -1, "Liberation Sans, Arial, Helvetica, Aura Sans"},
    {"Verdana", -1, "Tahoma, DejaVu Sans, Geneva, Aura Sans"},
    {"Tahoma", -1, "Verdana, DejaVu Sans, Geneva, Aura Sans"},
    {"Geneva", -1, "Verdana, DejaVu Sans, Helvetica, Aura Sans"},
    {"Trebuchet MS", -1, "Lucida Grande, Verdana, Inter, Aura Sans"},
    {"Lucida Grande", -1, "Lucida Sans Unicode, Lucida Sans, Geneva, Verdana, DejaVu Sans"},
    {"Lucida Sans Unicode", -1, "Lucida Grande, Lucida Sans, Geneva, Verdana, DejaVu Sans"},
    {"Lucida Sans", -1, "Lucida Grande, Lucida Sans Unicode, Geneva, Verdana, DejaVu Sans"},
    {"Segoe UI", -1, SANS_MODERN},
    {"Roboto", -1, SANS_MODERN},
    {"Open Sans", -1, SANS_MODERN},
    {"Lato", -1, SANS_MODERN},
    {"Noto Sans", -1, SANS_MODERN},
    {"Ubuntu", -1, SANS_MODERN},
    {"Source Sans Pro", -1, SANS_MODERN},
    {"Source Sans 3", -1, SANS_MODERN},
    {"Montserrat", -1, SANS_MODERN},
    {"Calibri", -1, "Carlito, " SANS_MODERN},
    {"Cantarell", -1, SANS_MODERN},
    {"Fira Sans", -1, SANS_MODERN},
    {"Oxygen", -1, SANS_MODERN},
    {"-apple-system", -1, "SF Pro Text, " SANS_MODERN},
    {"BlinkMacSystemFont", -1, "SF Pro Text, " SANS_MODERN},
    {"San Francisco", -1, "SF Pro Text, " SANS_MODERN},
    {"SF Pro", -1, "SF Pro Text, " SANS_MODERN},
    {"SF Pro Text", -1, SANS_MODERN},
    {"SF Pro Display", -1, SANS_MODERN},
    {".SFNSText", -1, SANS_MODERN},
    {"Inter", -1, "Inter Variable, Aura Sans"},
    {"Inter Variable", -1, "Inter, Aura Sans"},
    {"DejaVu Sans", -1, "Verdana, Aura Sans"},

    /* Serif. */
    {"Times", -1, "Times New Roman, Liberation Serif, Tinos, Aura Serif"},
    {"Times New Roman", -1, "Times, Liberation Serif, Tinos, Aura Serif"},
    {"Liberation Serif", -1, "Tinos, Times New Roman, Times, Aura Serif"},
    {"Tinos", -1, "Liberation Serif, Times New Roman, Times, Aura Serif"},
    {"Georgia", -1, "Gelasio, Times New Roman, Times, Aura Serif"},
    {"Palatino", -1, "Palatino Linotype, Book Antiqua, Georgia, Times New Roman, Aura Serif"},
    {"Palatino Linotype", -1, "Palatino, Book Antiqua, Georgia, Times New Roman, Aura Serif"},
    {"Book Antiqua", -1, "Palatino, Palatino Linotype, Georgia, Times New Roman, Aura Serif"},
    {"Garamond", -1, "EB Garamond, Adobe Garamond Pro, Palatino, Georgia, Times New Roman, Aura Serif"},
    {"Cambria", -1, "Caladea, Georgia, Times New Roman, Aura Serif"},
    {"Noto Serif", -1, "Georgia, DejaVu Serif, Times New Roman, Aura Serif"},
    {"DejaVu Serif", -1, "Georgia, Times New Roman, Aura Serif"},
    {"New York", -1, "Georgia, Times New Roman, Aura Serif"},

    /* Monospace. */
    {"Courier", -1, "Courier New, Liberation Mono, Cousine, Aura Mono"},
    {"Courier New", -1, "Courier, Liberation Mono, Cousine, Aura Mono"},
    {"Liberation Mono", -1, "Cousine, Courier New, Courier, Aura Mono"},
    {"Cousine", -1, "Liberation Mono, Courier New, Courier, Aura Mono"},
    {"Monaco", -1, "Menlo, Andale Mono, DejaVu Sans Mono, Courier New, Aura Mono"},
    {"Menlo", -1, "Monaco, DejaVu Sans Mono, Andale Mono, Courier New, Aura Mono"},
    {"Consolas", -1, MONO_MODERN},
    {"SFMono-Regular", -1, "SF Mono, " MONO_MODERN},
    {"SF Mono", -1, MONO_MODERN},
    {"Lucida Console", -1, "Monaco, Andale Mono, Courier New, Aura Mono"},
    {"Andale Mono", -1, "Monaco, DejaVu Sans Mono, Courier New, Aura Mono"},
    {"DejaVu Sans Mono", -1, "Menlo, Monaco, Andale Mono, Courier New, Aura Mono"},
    {"Source Code Pro", -1, MONO_MODERN},
    {"Roboto Mono", -1, MONO_MODERN},
    {"Fira Code", -1, MONO_MODERN},
    {"Fira Mono", -1, MONO_MODERN},
    {"JetBrains Mono", -1, MONO_MODERN},
    {"Ubuntu Mono", -1, MONO_MODERN},

    /* Emoji fonts pages name at the end of their stacks. */
    {"Apple Color Emoji", -1, EMOJI},
    {"Segoe UI Emoji", -1, EMOJI},
    {"Segoe UI Symbol", -1, "DejaVu Sans, " EMOJI},
    {"Noto Color Emoji", -1, EMOJI},
    {"Android Emoji", -1, EMOJI},
    {"Twemoji Mozilla", -1, "Noto Emoji"},
    {"Twemoji", -1, EMOJI},
    {"EmojiOne Color", -1, EMOJI},
};

/* Faces tried, in order, for a character no family of the page's list
 * has. CJK system faces are here for machines that have them; Aura does
 * not bundle one. */
static const char text_fallbacks[] = "DejaVu Sans, Lucida Grande, Arial Unicode MS, Symbola, Osaka, Hiragino Sans, "
                                     "Hiragino Kaku Gothic ProN, PingFang SC, Apple SD Gothic Neo, AppleGothic, "
                                     "Apple LiGothic, STHeiti, MS Gothic, SimSun, Gulim";
/* Faces tried first for a character with emoji presentation. */
static const char emoji_fallbacks[] = EMOJI ", Symbola";

const AuFontAlias *au_font_generic(const char *family, size_t len)
{
    for (size_t i = 0; i < sizeof generics / sizeof generics[0]; i++)
        if (au_ieq(generics[i].name, strlen(generics[i].name), family, len))
            return &generics[i];
    return NULL;
}

const AuFontAlias *au_font_alias(const char *family, size_t len)
{
    const AuFontAlias *g = au_font_generic(family, len);
    if (g)
        return g;
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        if (au_ieq(rows[i].name, strlen(rows[i].name), family, len))
            return &rows[i];
    return NULL;
}

const char *au_font_text_fallbacks(void)
{
    return text_fallbacks;
}

const char *au_font_emoji_fallbacks(void)
{
    return emoji_fallbacks;
}
