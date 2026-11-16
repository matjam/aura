/* Name tables: keywords, properties, aliases, flow-relative mapping and
 * shorthand membership. au_css_init interns every name and builds the
 * atom-to-id maps. */
#include "css_int.h"

#include <stddef.h>

/* ---- Keywords ---- */

static const char *const kw_names[AU_K_COUNT] = {
    "",
#define KW(id, str) str,
#include "keywords.def"
#undef KW
};

AuAtom css_kw_atoms[AU_K_COUNT];
AuAtom css_hn[CSS_HN_COUNT];

static const char *const hn_names[CSS_HN_COUNT] = {
#define X(n, s) s,
    CSS_HTML_NAMES(X)
#undef X
};

static uint16_t *kw_of_atom, *prop_of_atom;
static uint32_t kw_map_n, prop_map_n;

/* ---- Keyword lists of T_KW properties ---- */

#define KWL_ITEM(list, e, k) AU_K_##k,
#define KWL_ARR(list) static const uint16_t kwl_##list[] = {AU_KWL_##list(KWL_ITEM)};
AU_KWL_ALL(KWL_ARR)
#undef KWL_ARR

/* ---- Property table ---- */

/* Parse functions of T_BLOB and T_SPECIAL properties (propparse.c). */
#define PDECL_T_KW(x)
#define PDECL_T_LEN(x)
#define PDECL_T_PX(x)
#define PDECL_T_COLOR(x)
#define PDECL_T_NUM(x)
#define PDECL_T_INT(x)
#define PDECL_T_BLOB(x) AuCssValue *pp_##x(CssParseCtx *, uint16_t, AuCssValue *);
#define PDECL_T_SPECIAL(x) AuCssValue *pp_##x(CssParseCtx *, uint16_t, AuCssValue *);
#define P(id, name, type, flags, group, field, extra, initial) PDECL_##type(extra)
#include "props.def"

#define GT_INH AuStyleInh
#define GT_INHR AuStyleInhRare
#define GT_BOX AuStyleBox
#define GT_SURR AuStyleSurround
#define GT_BG AuStyleBackground
#define GT_FLEX AuStyleFlex
#define GT_RARE AuStyleRare

#define PX_T_KW(x) .kws = kwl_##x, .nkws = (uint8_t)CSS_ARRAY_LEN(kwl_##x)
#define PX_T_LEN(x) .extra = (x)
#define PX_T_PX(x) .extra = (x)
#define PX_T_COLOR(x) .extra = (x)
#define PX_T_NUM(x) .extra = (x)
#define PX_T_INT(x) .extra = (x)
#define PX_T_BLOB(x) .parse = pp_##x
#define PX_T_SPECIAL(x) .parse = pp_##x

const CssPropInfo css_props[AU_PROP_LONGHAND_END] = {
#define P(id, nm, ty, fl, gr, fi, ex, in)                                                                \
    [AU_PROP_##id] = {.name = nm, .type = ty, .flags = fl, .group = G_##gr,                           \
                      .offset = (uint16_t)offsetof(GT_##gr, fi), PX_##ty(ex), .initial = in},
#include "props.def"
};

static const char *const other_names[AU_PROP_COUNT - AU_PROP_LONGHAND_END] = {
#define L(id, name, lgroup, side) [AU_PROP_##id - AU_PROP_LONGHAND_END] = name,
#define S(id, name, expander) [AU_PROP_##id - AU_PROP_LONGHAND_END] = name,
#include "props.def"
};

static const struct {
    const char *name;
    uint16_t prop;
} aliases[] = {
#define A(alias, id) {alias, AU_PROP_##id},
#include "props.def"
};

static const struct {
    uint8_t group, side;
} logical_info[AU_PROP_LOGICAL_END - AU_PROP_LONGHAND_END] = {
#define L(id, name, lgroup, side) [AU_PROP_##id - AU_PROP_LONGHAND_END] = {lgroup, side},
#include "props.def"
};

const char *css_prop_name(uint16_t prop)
{
    if (prop > 0 && prop < AU_PROP_LONGHAND_END)
        return css_props[prop].name;
    if (prop >= AU_PROP_LONGHAND_END && prop < AU_PROP_COUNT)
        return other_names[prop - AU_PROP_LONGHAND_END];
    return "";
}

const char *au_css_property_name(AuCssProp p) { return css_prop_name((uint16_t)p); }

/* ---- Initialization ---- */

static void map_set(uint16_t **map, uint32_t *n, AuAtom a, uint16_t v)
{
    if (a >= *n) {
        uint32_t nn = *n ? *n : 1024;
        while (nn <= a)
            nn *= 2;
        *map = au_realloc(*map, nn * sizeof(uint16_t));
        memset(*map + *n, 0, (nn - *n) * sizeof(uint16_t));
        *n = nn;
    }
    (*map)[a] = v;
}

static void release_element_style(void *style) { au_style_release(style); }

void au_css_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    /* an element freed after it left its document frees its style */
    au_dom_set_style_release(release_element_style);
    /* the tables below keep atoms for good, whichever document is first */
    AuAtomBits prev = au_atom_scope_enter(0);
    for (int k = 1; k < AU_K_COUNT; k++) {
        AuAtom a = au_atom_cstr(kw_names[k]);
        css_kw_atoms[k] = a;
        if (a < kw_map_n && kw_of_atom[a])
            continue; /* the first id of a duplicated string wins */
        map_set(&kw_of_atom, &kw_map_n, a, (uint16_t)k);
    }
    for (int p = 1; p < AU_PROP_COUNT; p++)
        map_set(&prop_of_atom, &prop_map_n, au_atom_cstr(css_prop_name((uint16_t)p)), (uint16_t)p);
    for (size_t i = 0; i < CSS_ARRAY_LEN(aliases); i++)
        map_set(&prop_of_atom, &prop_map_n, au_atom_cstr(aliases[i].name), aliases[i].prop);
    for (int i = 0; i < CSS_HN_COUNT; i++)
        css_hn[i] = au_atom_cstr(hn_names[i]);
    css_style_init_defaults();
    au_atom_scope_leave(prev);
}

uint16_t css_kw_lookup(const char *s, size_t n)
{
    char buf[64];
    if (!css_lower(s, n, buf, sizeof buf))
        return 0;
    AuAtom a = au_atom_lookup(buf, n);
    return a && a < kw_map_n ? kw_of_atom[a] : 0;
}

uint16_t css_prop_lookup(const char *s, size_t n)
{
    char buf[64];
    if (!css_lower(s, n, buf, sizeof buf))
        return 0;
    AuAtom a = au_atom_lookup(buf, n);
    return a && a < prop_map_n ? prop_of_atom[a] : 0;
}

bool css_ident_is_custom_prop(const char *s, size_t n) { return n >= 2 && s[0] == '-' && s[1] == '-'; }

AuCssProp au_css_property_id(AuAtom name)
{
    au_css_init();
    if (name < prop_map_n && prop_of_atom[name])
        return (AuCssProp)prop_of_atom[name];
    return css_prop_lookup(au_atom_str(name), au_atom_len(name));
}

AuAtom au_css_keyword_atom(AuCssKeyword k)
{
    au_css_init();
    return k > 0 && k < AU_K_COUNT ? css_kw_atoms[k] : 0;
}

const char *au_css_keyword_name(AuCssKeyword k) { return k > 0 && k < AU_K_COUNT ? kw_names[k] : ""; }

AuCssKeyword au_css_keyword_of(AuAtom a)
{
    au_css_init();
    return (AuCssKeyword)(a < kw_map_n ? kw_of_atom[a] : 0);
}

/* ---- Keyword properties ---- */

/* The keyword list of a property; T_SPECIAL properties with a keyword
 * enum have theirs here. */
static const uint16_t *prop_kws(uint16_t prop, int *n)
{
    if (prop == AU_PROP_cursor) {
        *n = (int)CSS_ARRAY_LEN(kwl_CURSOR);
        return kwl_CURSOR;
    }
    if (prop == AU_PROP_font_style) {
        *n = (int)CSS_ARRAY_LEN(kwl_FONT_STYLE);
        return kwl_FONT_STYLE;
    }
    *n = prop < AU_PROP_LONGHAND_END ? css_props[prop].nkws : 0;
    return prop < AU_PROP_LONGHAND_END ? css_props[prop].kws : NULL;
}

uint16_t css_prop_kw_at(uint16_t prop, int idx)
{
    int n;
    const uint16_t *k = prop_kws(prop, &n);
    return idx >= 0 && idx < n ? k[idx] : 0;
}

int css_prop_kw_index(uint16_t prop, uint16_t kw)
{
    int nk;
    const uint16_t *kws = prop_kws(prop, &nk);
    for (int i = 0; i < nk; i++)
        if (kws[i] == kw)
            return i;
    /* legacy and vendor values mapped to their standard equivalents */
    switch (prop) {
    case AU_PROP_position:
        if (kw == AU_K__webkit_sticky)
            return AU_POSITION_STICKY;
        break;
    case AU_PROP_overflow_x:
    case AU_PROP_overflow_y:
        if (kw == AU_K_overlay)
            return AU_OVERFLOW_AUTO;
        break;
    case AU_PROP_text_align:
        if (kw == AU_K__moz_left)
            return AU_TEXT_ALIGN_WEBKIT_LEFT;
        if (kw == AU_K__moz_right)
            return AU_TEXT_ALIGN_WEBKIT_RIGHT;
        if (kw == AU_K__moz_center)
            return AU_TEXT_ALIGN_WEBKIT_CENTER;
        if (kw == AU_K_justify_all)
            return AU_TEXT_ALIGN_JUSTIFY;
        break;
    case AU_PROP_writing_mode:
        if (kw == AU_K_lr || kw == AU_K_lr_tb || kw == AU_K_rl || kw == AU_K_rl_tb)
            return AU_WRITING_MODE_HORIZONTAL_TB;
        if (kw == AU_K_tb || kw == AU_K_tb_rl)
            return AU_WRITING_MODE_VERTICAL_RL;
        break;
    case AU_PROP_cursor:
        if (kw == AU_K__webkit_grab)
            return AU_CURSOR_GRAB;
        if (kw == AU_K__webkit_grabbing)
            return AU_CURSOR_GRABBING;
        if (kw == AU_K__webkit_zoom_in)
            return AU_CURSOR_ZOOM_IN;
        if (kw == AU_K__webkit_zoom_out)
            return AU_CURSOR_ZOOM_OUT;
        if (kw == AU_K_hand)
            return AU_CURSOR_POINTER;
        break;
    case AU_PROP_image_rendering:
        if (kw == AU_K_optimizespeed)
            return AU_IMAGE_RENDERING_PIXELATED;
        if (kw == AU_K_optimizequality)
            return AU_IMAGE_RENDERING_SMOOTH;
        if (kw == AU_K__webkit_optimize_contrast)
            return AU_IMAGE_RENDERING_CRISP_EDGES;
        break;
    default:
        break;
    }
    return -1;
}

/* ---- Flow-relative properties ---- */

static const uint16_t lgroup_props[][4] = {
    [LG_MARGIN] = {AU_PROP_margin_top, AU_PROP_margin_right, AU_PROP_margin_bottom, AU_PROP_margin_left},
    [LG_PADDING] = {AU_PROP_padding_top, AU_PROP_padding_right, AU_PROP_padding_bottom, AU_PROP_padding_left},
    [LG_INSET] = {AU_PROP_top, AU_PROP_right, AU_PROP_bottom, AU_PROP_left},
    [LG_BORDER_WIDTH] = {AU_PROP_border_top_width, AU_PROP_border_right_width, AU_PROP_border_bottom_width,
                         AU_PROP_border_left_width},
    [LG_BORDER_STYLE] = {AU_PROP_border_top_style, AU_PROP_border_right_style, AU_PROP_border_bottom_style,
                         AU_PROP_border_left_style},
    [LG_BORDER_COLOR] = {AU_PROP_border_top_color, AU_PROP_border_right_color, AU_PROP_border_bottom_color,
                         AU_PROP_border_left_color},
    [LG_SIZE] = {AU_PROP_width, AU_PROP_height, 0, 0},
    [LG_MIN_SIZE] = {AU_PROP_min_width, AU_PROP_min_height, 0, 0},
    [LG_MAX_SIZE] = {AU_PROP_max_width, AU_PROP_max_height, 0, 0},
    [LG_RADIUS] = {AU_PROP_border_top_left_radius, AU_PROP_border_top_right_radius,
                   AU_PROP_border_bottom_right_radius, AU_PROP_border_bottom_left_radius},
};

enum { SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM, SIDE_LEFT };

/* The physical side of a flow-relative side. */
static int phys_side(int side, int wm, int dir)
{
    bool rtl = dir == AU_DIRECTION_RTL;
    int bs, is;
    switch (wm) {
    case AU_WRITING_MODE_VERTICAL_RL:
    case AU_WRITING_MODE_SIDEWAYS_RL:
        bs = SIDE_RIGHT;
        is = rtl ? SIDE_BOTTOM : SIDE_TOP;
        break;
    case AU_WRITING_MODE_VERTICAL_LR:
        bs = SIDE_LEFT;
        is = rtl ? SIDE_BOTTOM : SIDE_TOP;
        break;
    case AU_WRITING_MODE_SIDEWAYS_LR:
        bs = SIDE_LEFT;
        is = rtl ? SIDE_TOP : SIDE_BOTTOM;
        break;
    default:
        bs = SIDE_TOP;
        is = rtl ? SIDE_RIGHT : SIDE_LEFT;
        break;
    }
    switch (side) {
    case LS_BLOCK_START: return bs;
    case LS_BLOCK_END: return (bs + 2) & 3;
    case LS_INLINE_START: return is;
    default: return (is + 2) & 3;
    }
}

static int corner(int a, int b)
{
    int v = (a == SIDE_TOP || a == SIDE_BOTTOM) ? a : b;
    int h = (a == SIDE_LEFT || a == SIDE_RIGHT) ? a : b;
    if (v == SIDE_TOP)
        return h == SIDE_LEFT ? 0 : 1;
    return h == SIDE_RIGHT ? 2 : 3;
}

uint16_t css_logical_to_physical(uint16_t prop, int wm, int dir)
{
    if (prop < AU_PROP_LONGHAND_END || prop >= AU_PROP_LOGICAL_END)
        return prop;
    int g = logical_info[prop - AU_PROP_LONGHAND_END].group;
    int side = logical_info[prop - AU_PROP_LONGHAND_END].side;
    bool horizontal = wm == AU_WRITING_MODE_HORIZONTAL_TB;
    switch (side) {
    case LS_INLINE:
        return lgroup_props[g][horizontal ? 0 : 1];
    case LS_BLOCK:
        return lgroup_props[g][horizontal ? 1 : 0];
    case LS_START_START:
        return lgroup_props[g][corner(phys_side(LS_BLOCK_START, wm, dir), phys_side(LS_INLINE_START, wm, dir))];
    case LS_START_END:
        return lgroup_props[g][corner(phys_side(LS_BLOCK_START, wm, dir), phys_side(LS_INLINE_END, wm, dir))];
    case LS_END_START:
        return lgroup_props[g][corner(phys_side(LS_BLOCK_END, wm, dir), phys_side(LS_INLINE_START, wm, dir))];
    case LS_END_END:
        return lgroup_props[g][corner(phys_side(LS_BLOCK_END, wm, dir), phys_side(LS_INLINE_END, wm, dir))];
    default:
        return lgroup_props[g][phys_side(side, wm, dir)];
    }
}

uint16_t css_logical_template(uint16_t prop)
{
    if (prop < AU_PROP_LONGHAND_END || prop >= AU_PROP_LOGICAL_END)
        return prop;
    return lgroup_props[logical_info[prop - AU_PROP_LONGHAND_END].group][0];
}

/* ---- Shorthand membership ---- */

#define LH(...) (const uint16_t[]){__VA_ARGS__, 0}
#define SIDES(p) AU_PROP_##p##_top, AU_PROP_##p##_right, AU_PROP_##p##_bottom, AU_PROP_##p##_left
#define BSIDE(s) AU_PROP_border_##s##_width, AU_PROP_border_##s##_style, AU_PROP_border_##s##_color

static const uint16_t *const sh_longhands[AU_PROP_COUNT - AU_PROP_LOGICAL_END] = {
#define SH(id) [AU_PROP_##id - AU_PROP_LOGICAL_END]
    SH(margin) = LH(SIDES(margin)),
    SH(padding) = LH(SIDES(padding)),
    SH(inset) = LH(AU_PROP_top, AU_PROP_right, AU_PROP_bottom, AU_PROP_left),
    SH(border_width) = LH(AU_PROP_border_top_width, AU_PROP_border_right_width, AU_PROP_border_bottom_width,
                          AU_PROP_border_left_width),
    SH(border_style) = LH(AU_PROP_border_top_style, AU_PROP_border_right_style, AU_PROP_border_bottom_style,
                          AU_PROP_border_left_style),
    SH(border_color) = LH(AU_PROP_border_top_color, AU_PROP_border_right_color, AU_PROP_border_bottom_color,
                          AU_PROP_border_left_color),
    SH(border) = LH(BSIDE(top), BSIDE(right), BSIDE(bottom), BSIDE(left)),
    SH(border_top) = LH(BSIDE(top)),
    SH(border_right) = LH(BSIDE(right)),
    SH(border_bottom) = LH(BSIDE(bottom)),
    SH(border_left) = LH(BSIDE(left)),
    SH(border_radius) = LH(AU_PROP_border_top_left_radius, AU_PROP_border_top_right_radius,
                           AU_PROP_border_bottom_right_radius, AU_PROP_border_bottom_left_radius),
    SH(outline) = LH(AU_PROP_outline_color, AU_PROP_outline_style, AU_PROP_outline_width),
    SH(background) = LH(AU_PROP_background_image, AU_PROP_background_position_x, AU_PROP_background_position_y,
                        AU_PROP_background_size, AU_PROP_background_repeat, AU_PROP_background_attachment,
                        AU_PROP_background_origin, AU_PROP_background_clip, AU_PROP_background_color),
    SH(background_position) = LH(AU_PROP_background_position_x, AU_PROP_background_position_y),
    SH(mask) = LH(AU_PROP_mask_image, AU_PROP_mask_position_x, AU_PROP_mask_position_y, AU_PROP_mask_size,
                  AU_PROP_mask_repeat, AU_PROP_mask_mode, AU_PROP_mask_origin, AU_PROP_mask_clip),
    SH(mask_position) = LH(AU_PROP_mask_position_x, AU_PROP_mask_position_y),
    SH(font) = LH(AU_PROP_font_style, AU_PROP_font_variant_caps, AU_PROP_font_weight, AU_PROP_font_stretch,
                  AU_PROP_font_size, AU_PROP_line_height, AU_PROP_font_family, AU_PROP_font_kerning),
    SH(font_variant) = LH(AU_PROP_font_variant_caps),
    SH(list_style) = LH(AU_PROP_list_style_position, AU_PROP_list_style_image, AU_PROP_list_style_type),
    SH(text_decoration) = LH(AU_PROP_text_decoration_line, AU_PROP_text_decoration_thickness,
                             AU_PROP_text_decoration_style, AU_PROP_text_decoration_color),
    SH(flex) = LH(AU_PROP_flex_grow, AU_PROP_flex_shrink, AU_PROP_flex_basis),
    SH(flex_flow) = LH(AU_PROP_flex_direction, AU_PROP_flex_wrap),
    SH(gap) = LH(AU_PROP_row_gap, AU_PROP_column_gap),
    SH(place_content) = LH(AU_PROP_align_content, AU_PROP_justify_content),
    SH(place_items) = LH(AU_PROP_align_items, AU_PROP_justify_items),
    SH(place_self) = LH(AU_PROP_align_self, AU_PROP_justify_self),
    SH(grid_template) = LH(AU_PROP_grid_template_rows, AU_PROP_grid_template_columns, AU_PROP_grid_template_areas),
    SH(grid) = LH(AU_PROP_grid_template_rows, AU_PROP_grid_template_columns, AU_PROP_grid_template_areas,
                  AU_PROP_grid_auto_rows, AU_PROP_grid_auto_columns, AU_PROP_grid_auto_flow),
    SH(grid_row) = LH(AU_PROP_grid_row_start, AU_PROP_grid_row_end),
    SH(grid_column) = LH(AU_PROP_grid_column_start, AU_PROP_grid_column_end),
    SH(grid_area) = LH(AU_PROP_grid_row_start, AU_PROP_grid_column_start, AU_PROP_grid_row_end,
                       AU_PROP_grid_column_end),
    SH(transition) = LH(AU_PROP_transition_property, AU_PROP_transition_duration,
                        AU_PROP_transition_timing_function, AU_PROP_transition_delay, AU_PROP_transition_behavior),
    SH(animation) = LH(AU_PROP_animation_name, AU_PROP_animation_duration, AU_PROP_animation_timing_function,
                       AU_PROP_animation_delay, AU_PROP_animation_iteration_count, AU_PROP_animation_direction,
                       AU_PROP_animation_fill_mode, AU_PROP_animation_play_state),
    SH(white_space) = LH(AU_PROP_white_space_collapse, AU_PROP_text_wrap_mode),
    SH(text_wrap) = LH(AU_PROP_text_wrap_mode, AU_PROP_text_wrap_style),
    SH(overflow) = LH(AU_PROP_overflow_x, AU_PROP_overflow_y),
    SH(container) = LH(AU_PROP_container_name, AU_PROP_container_type),
    SH(margin_block) = LH(AU_PROP_margin_block_start, AU_PROP_margin_block_end),
    SH(margin_inline) = LH(AU_PROP_margin_inline_start, AU_PROP_margin_inline_end),
    SH(padding_block) = LH(AU_PROP_padding_block_start, AU_PROP_padding_block_end),
    SH(padding_inline) = LH(AU_PROP_padding_inline_start, AU_PROP_padding_inline_end),
    SH(inset_block) = LH(AU_PROP_inset_block_start, AU_PROP_inset_block_end),
    SH(inset_inline) = LH(AU_PROP_inset_inline_start, AU_PROP_inset_inline_end),
    SH(border_block_width) = LH(AU_PROP_border_block_start_width, AU_PROP_border_block_end_width),
    SH(border_inline_width) = LH(AU_PROP_border_inline_start_width, AU_PROP_border_inline_end_width),
    SH(border_block_style) = LH(AU_PROP_border_block_start_style, AU_PROP_border_block_end_style),
    SH(border_inline_style) = LH(AU_PROP_border_inline_start_style, AU_PROP_border_inline_end_style),
    SH(border_block_color) = LH(AU_PROP_border_block_start_color, AU_PROP_border_block_end_color),
    SH(border_inline_color) = LH(AU_PROP_border_inline_start_color, AU_PROP_border_inline_end_color),
    SH(border_block) = LH(AU_PROP_border_block_start_width, AU_PROP_border_block_start_style,
                          AU_PROP_border_block_start_color, AU_PROP_border_block_end_width,
                          AU_PROP_border_block_end_style, AU_PROP_border_block_end_color),
    SH(border_inline) = LH(AU_PROP_border_inline_start_width, AU_PROP_border_inline_start_style,
                           AU_PROP_border_inline_start_color, AU_PROP_border_inline_end_width,
                           AU_PROP_border_inline_end_style, AU_PROP_border_inline_end_color),
    SH(border_block_start) = LH(AU_PROP_border_block_start_width, AU_PROP_border_block_start_style,
                                AU_PROP_border_block_start_color),
    SH(border_block_end) = LH(AU_PROP_border_block_end_width, AU_PROP_border_block_end_style,
                              AU_PROP_border_block_end_color),
    SH(border_inline_start) = LH(AU_PROP_border_inline_start_width, AU_PROP_border_inline_start_style,
                                 AU_PROP_border_inline_start_color),
    SH(border_inline_end) = LH(AU_PROP_border_inline_end_width, AU_PROP_border_inline_end_style,
                               AU_PROP_border_inline_end_color),
#undef SH
};

static uint16_t all_longhands[AU_PROP_LONGHAND_END];

const uint16_t *css_shorthand_longhands(uint16_t sh)
{
    if (sh == AU_PROP_all) {
        if (!all_longhands[0]) {
            /* all: every longhand except direction and unicode-bidi */
            int k = 0;
            for (int p = 1; p < AU_PROP_LONGHAND_END; p++)
                if (p != AU_PROP_direction && p != AU_PROP_unicode_bidi)
                    all_longhands[k++] = (uint16_t)p;
            all_longhands[k] = 0;
        }
        return all_longhands;
    }
    if (sh < AU_PROP_LOGICAL_END || sh >= AU_PROP_COUNT)
        return NULL;
    return sh_longhands[sh - AU_PROP_LOGICAL_END];
}
