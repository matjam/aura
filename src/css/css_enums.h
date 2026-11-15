/* Keyword enums of the computed style.
 *
 * Each AU_KWL_<NAME>(X) list expands X(NAME, ENUM, keyword) once per value,
 * in enum order. css.h turns each list into an enum (AU_POSITION_STATIC and
 * so on); src/css/props.c turns it into the keyword table the parser uses,
 * so the two cannot drift apart. The keyword is an AU_K_* id from
 * keywords.def. Included by css.h. */
#ifndef AURA_CSS_ENUMS_H
#define AURA_CSS_ENUMS_H

#define AU_KWL_FONT_STYLE(X) X(FONT_STYLE, NORMAL, normal) X(FONT_STYLE, ITALIC, italic) X(FONT_STYLE, OBLIQUE, oblique)
#define AU_KWL_FONT_VARIANT_CAPS(X)                                                                        \
    X(FONT_VARIANT_CAPS, NORMAL, normal) X(FONT_VARIANT_CAPS, SMALL_CAPS, small_caps)                     \
    X(FONT_VARIANT_CAPS, ALL_SMALL_CAPS, all_small_caps) X(FONT_VARIANT_CAPS, PETITE_CAPS, petite_caps)   \
    X(FONT_VARIANT_CAPS, ALL_PETITE_CAPS, all_petite_caps) X(FONT_VARIANT_CAPS, UNICASE, unicase)         \
    X(FONT_VARIANT_CAPS, TITLING_CAPS, titling_caps)
#define AU_KWL_TEXT_ALIGN(X)                                                                                \
    X(TEXT_ALIGN, START, start) X(TEXT_ALIGN, END, end) X(TEXT_ALIGN, LEFT, left) X(TEXT_ALIGN, RIGHT, right) \
    X(TEXT_ALIGN, CENTER, center) X(TEXT_ALIGN, JUSTIFY, justify) X(TEXT_ALIGN, MATCH_PARENT, match_parent) \
    X(TEXT_ALIGN, WEBKIT_LEFT, _webkit_left) X(TEXT_ALIGN, WEBKIT_RIGHT, _webkit_right)                     \
    X(TEXT_ALIGN, WEBKIT_CENTER, _webkit_center)
#define AU_KWL_WHITE_SPACE_COLLAPSE(X)                                                             \
    X(WHITE_SPACE_COLLAPSE, COLLAPSE, collapse) X(WHITE_SPACE_COLLAPSE, PRESERVE, preserve)       \
    X(WHITE_SPACE_COLLAPSE, PRESERVE_BREAKS, preserve_breaks)                                     \
    X(WHITE_SPACE_COLLAPSE, PRESERVE_SPACES, preserve_spaces) X(WHITE_SPACE_COLLAPSE, BREAK_SPACES, break_spaces)
#define AU_KWL_TEXT_WRAP_MODE(X) X(TEXT_WRAP_MODE, WRAP, wrap) X(TEXT_WRAP_MODE, NOWRAP, nowrap)
#define AU_KWL_DIRECTION(X) X(DIRECTION, LTR, ltr) X(DIRECTION, RTL, rtl)
#define AU_KWL_WRITING_MODE(X)                                                                   \
    X(WRITING_MODE, HORIZONTAL_TB, horizontal_tb) X(WRITING_MODE, VERTICAL_RL, vertical_rl)     \
    X(WRITING_MODE, VERTICAL_LR, vertical_lr) X(WRITING_MODE, SIDEWAYS_RL, sideways_rl)         \
    X(WRITING_MODE, SIDEWAYS_LR, sideways_lr)
#define AU_KWL_VISIBILITY(X) X(VISIBILITY, VISIBLE, visible) X(VISIBILITY, HIDDEN, hidden) X(VISIBILITY, COLLAPSE, collapse)
#define AU_KWL_LIST_STYLE_POSITION(X) X(LIST_STYLE_POSITION, OUTSIDE, outside) X(LIST_STYLE_POSITION, INSIDE, inside)
#define AU_KWL_TEXT_TRANSFORM(X)                                                                    \
    X(TEXT_TRANSFORM, NONE, none) X(TEXT_TRANSFORM, CAPITALIZE, capitalize)                        \
    X(TEXT_TRANSFORM, UPPERCASE, uppercase) X(TEXT_TRANSFORM, LOWERCASE, lowercase)                \
    X(TEXT_TRANSFORM, FULL_WIDTH, full_width) X(TEXT_TRANSFORM, FULL_SIZE_KANA, full_size_kana)
#define AU_KWL_TEXT_ALIGN_LAST(X)                                                                    \
    X(TEXT_ALIGN_LAST, AUTO, auto_) X(TEXT_ALIGN_LAST, START, start) X(TEXT_ALIGN_LAST, END, end)   \
    X(TEXT_ALIGN_LAST, LEFT, left) X(TEXT_ALIGN_LAST, RIGHT, right) X(TEXT_ALIGN_LAST, CENTER, center) \
    X(TEXT_ALIGN_LAST, JUSTIFY, justify) X(TEXT_ALIGN_LAST, MATCH_PARENT, match_parent)
#define AU_KWL_TEXT_WRAP_STYLE(X)                                                                  \
    X(TEXT_WRAP_STYLE, AUTO, auto_) X(TEXT_WRAP_STYLE, BALANCE, balance)                           \
    X(TEXT_WRAP_STYLE, STABLE, stable) X(TEXT_WRAP_STYLE, PRETTY, pretty)
#define AU_KWL_WORD_BREAK(X)                                                                        \
    X(WORD_BREAK, NORMAL, normal) X(WORD_BREAK, BREAK_ALL, break_all) X(WORD_BREAK, KEEP_ALL, keep_all) \
    X(WORD_BREAK, BREAK_WORD, break_word) X(WORD_BREAK, AUTO_PHRASE, auto_phrase)
#define AU_KWL_OVERFLOW_WRAP(X) \
    X(OVERFLOW_WRAP, NORMAL, normal) X(OVERFLOW_WRAP, BREAK_WORD, break_word) X(OVERFLOW_WRAP, ANYWHERE, anywhere)
#define AU_KWL_HYPHENS(X) X(HYPHENS, NONE, none) X(HYPHENS, MANUAL, manual) X(HYPHENS, AUTO, auto_)
#define AU_KWL_LINE_BREAK(X)                                                                       \
    X(LINE_BREAK, AUTO, auto_) X(LINE_BREAK, LOOSE, loose) X(LINE_BREAK, NORMAL, normal)           \
    X(LINE_BREAK, STRICT, strict) X(LINE_BREAK, ANYWHERE, anywhere)
#define AU_KWL_BORDER_COLLAPSE(X) X(BORDER_COLLAPSE, SEPARATE, separate) X(BORDER_COLLAPSE, COLLAPSE, collapse)
#define AU_KWL_CAPTION_SIDE(X) X(CAPTION_SIDE, TOP, top) X(CAPTION_SIDE, BOTTOM, bottom)
#define AU_KWL_EMPTY_CELLS(X) X(EMPTY_CELLS, SHOW, show) X(EMPTY_CELLS, HIDE, hide)
#define AU_KWL_POINTER_EVENTS(X)                                                                     \
    X(POINTER_EVENTS, AUTO, auto_) X(POINTER_EVENTS, NONE, none)                                    \
    X(POINTER_EVENTS, VISIBLEPAINTED, visiblepainted) X(POINTER_EVENTS, VISIBLEFILL, visiblefill)   \
    X(POINTER_EVENTS, VISIBLESTROKE, visiblestroke) X(POINTER_EVENTS, VISIBLE, visible)             \
    X(POINTER_EVENTS, PAINTED, painted) X(POINTER_EVENTS, FILL, fill) X(POINTER_EVENTS, STROKE, stroke) \
    X(POINTER_EVENTS, ALL, all) X(POINTER_EVENTS, BOUNDING_BOX, bounding_box)
#define AU_KWL_CURSOR(X)                                                                             \
    X(CURSOR, AUTO, auto_) X(CURSOR, DEFAULT, default_) X(CURSOR, NONE, none)                        \
    X(CURSOR, CONTEXT_MENU, context_menu) X(CURSOR, HELP, help) X(CURSOR, POINTER, pointer)          \
    X(CURSOR, PROGRESS, progress) X(CURSOR, WAIT, wait) X(CURSOR, CELL, cell)                        \
    X(CURSOR, CROSSHAIR, crosshair) X(CURSOR, TEXT, text) X(CURSOR, VERTICAL_TEXT, vertical_text)    \
    X(CURSOR, ALIAS, alias) X(CURSOR, COPY, copy) X(CURSOR, MOVE, move) X(CURSOR, NO_DROP, no_drop)  \
    X(CURSOR, NOT_ALLOWED, not_allowed) X(CURSOR, GRAB, grab) X(CURSOR, GRABBING, grabbing)          \
    X(CURSOR, E_RESIZE, e_resize) X(CURSOR, N_RESIZE, n_resize) X(CURSOR, NE_RESIZE, ne_resize)      \
    X(CURSOR, NW_RESIZE, nw_resize) X(CURSOR, S_RESIZE, s_resize) X(CURSOR, SE_RESIZE, se_resize)    \
    X(CURSOR, SW_RESIZE, sw_resize) X(CURSOR, W_RESIZE, w_resize) X(CURSOR, EW_RESIZE, ew_resize)    \
    X(CURSOR, NS_RESIZE, ns_resize) X(CURSOR, NESW_RESIZE, nesw_resize)                              \
    X(CURSOR, NWSE_RESIZE, nwse_resize) X(CURSOR, COL_RESIZE, col_resize)                            \
    X(CURSOR, ROW_RESIZE, row_resize) X(CURSOR, ALL_SCROLL, all_scroll) X(CURSOR, ZOOM_IN, zoom_in)  \
    X(CURSOR, ZOOM_OUT, zoom_out)
#define AU_KWL_FONT_KERNING(X) X(FONT_KERNING, AUTO, auto_) X(FONT_KERNING, NORMAL, normal) X(FONT_KERNING, NONE, none)
#define AU_KWL_TEXT_RENDERING(X)                                                                   \
    X(TEXT_RENDERING, AUTO, auto_) X(TEXT_RENDERING, OPTIMIZESPEED, optimizespeed)                 \
    X(TEXT_RENDERING, OPTIMIZELEGIBILITY, optimizelegibility)                                      \
    X(TEXT_RENDERING, GEOMETRICPRECISION, geometricprecision)
#define AU_KWL_IMAGE_RENDERING(X)                                                                  \
    X(IMAGE_RENDERING, AUTO, auto_) X(IMAGE_RENDERING, SMOOTH, smooth)                             \
    X(IMAGE_RENDERING, HIGH_QUALITY, high_quality) X(IMAGE_RENDERING, PIXELATED, pixelated)        \
    X(IMAGE_RENDERING, CRISP_EDGES, crisp_edges)
#define AU_KWL_POSITION(X)                                                                        \
    X(POSITION, STATIC, static_) X(POSITION, RELATIVE, relative) X(POSITION, ABSOLUTE, absolute)  \
    X(POSITION, FIXED, fixed) X(POSITION, STICKY, sticky)
#define AU_KWL_FLOAT(X)                                                                           \
    X(FLOAT, NONE, none) X(FLOAT, LEFT, left) X(FLOAT, RIGHT, right)                              \
    X(FLOAT, INLINE_START, inline_start) X(FLOAT, INLINE_END, inline_end)
#define AU_KWL_CLEAR(X)                                                                           \
    X(CLEAR, NONE, none) X(CLEAR, LEFT, left) X(CLEAR, RIGHT, right) X(CLEAR, BOTH, both)         \
    X(CLEAR, INLINE_START, inline_start) X(CLEAR, INLINE_END, inline_end)
#define AU_KWL_BOX_SIZING(X) X(BOX_SIZING, CONTENT_BOX, content_box) X(BOX_SIZING, BORDER_BOX, border_box)
#define AU_KWL_OVERFLOW(X)                                                                        \
    X(OVERFLOW, VISIBLE, visible) X(OVERFLOW, HIDDEN, hidden) X(OVERFLOW, CLIP, clip)             \
    X(OVERFLOW, SCROLL, scroll) X(OVERFLOW, AUTO, auto_)
#define AU_KWL_UNICODE_BIDI(X)                                                                    \
    X(UNICODE_BIDI, NORMAL, normal) X(UNICODE_BIDI, EMBED, embed) X(UNICODE_BIDI, ISOLATE, isolate) \
    X(UNICODE_BIDI, BIDI_OVERRIDE, bidi_override) X(UNICODE_BIDI, ISOLATE_OVERRIDE, isolate_override) \
    X(UNICODE_BIDI, PLAINTEXT, plaintext)
#define AU_KWL_TABLE_LAYOUT(X) X(TABLE_LAYOUT, AUTO, auto_) X(TABLE_LAYOUT, FIXED, fixed)
#define AU_KWL_BORDER_STYLE(X)                                                                    \
    X(BORDER_STYLE, NONE, none) X(BORDER_STYLE, HIDDEN, hidden) X(BORDER_STYLE, DOTTED, dotted)   \
    X(BORDER_STYLE, DASHED, dashed) X(BORDER_STYLE, SOLID, solid) X(BORDER_STYLE, DOUBLE, double_) \
    X(BORDER_STYLE, GROOVE, groove) X(BORDER_STYLE, RIDGE, ridge) X(BORDER_STYLE, INSET, inset)   \
    X(BORDER_STYLE, OUTSET, outset)
/* Same numbering as BORDER_STYLE, with AUTO after it. */
#define AU_KWL_OUTLINE_STYLE(X)                                                                   \
    X(OUTLINE_STYLE, NONE, none) X(OUTLINE_STYLE, HIDDEN, hidden) X(OUTLINE_STYLE, DOTTED, dotted) \
    X(OUTLINE_STYLE, DASHED, dashed) X(OUTLINE_STYLE, SOLID, solid) X(OUTLINE_STYLE, DOUBLE, double_) \
    X(OUTLINE_STYLE, GROOVE, groove) X(OUTLINE_STYLE, RIDGE, ridge) X(OUTLINE_STYLE, INSET, inset) \
    X(OUTLINE_STYLE, OUTSET, outset) X(OUTLINE_STYLE, AUTO, auto_)
#define AU_KWL_FLEX_DIRECTION(X)                                                                  \
    X(FLEX_DIRECTION, ROW, row) X(FLEX_DIRECTION, ROW_REVERSE, row_reverse)                       \
    X(FLEX_DIRECTION, COLUMN, column) X(FLEX_DIRECTION, COLUMN_REVERSE, column_reverse)
#define AU_KWL_FLEX_WRAP(X) X(FLEX_WRAP, NOWRAP, nowrap) X(FLEX_WRAP, WRAP, wrap) X(FLEX_WRAP, WRAP_REVERSE, wrap_reverse)
#define AU_KWL_TRANSFORM_STYLE(X) X(TRANSFORM_STYLE, FLAT, flat) X(TRANSFORM_STYLE, PRESERVE_3D, preserve_3d)
#define AU_KWL_BACKFACE_VISIBILITY(X) X(BACKFACE_VISIBILITY, VISIBLE, visible) X(BACKFACE_VISIBILITY, HIDDEN, hidden)
#define AU_KWL_OBJECT_FIT(X)                                                                      \
    X(OBJECT_FIT, FILL, fill) X(OBJECT_FIT, CONTAIN, contain) X(OBJECT_FIT, COVER, cover)         \
    X(OBJECT_FIT, NONE, none) X(OBJECT_FIT, SCALE_DOWN, scale_down)
#define AU_KWL_RESIZE(X)                                                                          \
    X(RESIZE, NONE, none) X(RESIZE, BOTH, both) X(RESIZE, HORIZONTAL, horizontal)                 \
    X(RESIZE, VERTICAL, vertical) X(RESIZE, BLOCK, block) X(RESIZE, INLINE, inline_)
#define AU_KWL_APPEARANCE(X)                                                                      \
    X(APPEARANCE, NONE, none) X(APPEARANCE, AUTO, auto_) X(APPEARANCE, MENULIST_BUTTON, menulist_button) \
    X(APPEARANCE, TEXTFIELD, textfield) X(APPEARANCE, BUTTON, button) X(APPEARANCE, CHECKBOX, checkbox) \
    X(APPEARANCE, RADIO, radio) X(APPEARANCE, LISTBOX, listbox) X(APPEARANCE, MENULIST, menulist) \
    X(APPEARANCE, METER, meter) X(APPEARANCE, PROGRESS_BAR, progress_bar)                         \
    X(APPEARANCE, PUSH_BUTTON, push_button) X(APPEARANCE, SEARCHFIELD, searchfield)               \
    X(APPEARANCE, SLIDER_HORIZONTAL, slider_horizontal) X(APPEARANCE, SQUARE_BUTTON, square_button) \
    X(APPEARANCE, TEXTAREA, textarea)
#define AU_KWL_SCROLL_BEHAVIOR(X) X(SCROLL_BEHAVIOR, AUTO, auto_) X(SCROLL_BEHAVIOR, SMOOTH, smooth)
#define AU_KWL_ISOLATION(X) X(ISOLATION, AUTO, auto_) X(ISOLATION, ISOLATE, isolate)
#define AU_KWL_MIX_BLEND_MODE(X)                                                                  \
    X(MIX_BLEND_MODE, NORMAL, normal) X(MIX_BLEND_MODE, MULTIPLY, multiply)                       \
    X(MIX_BLEND_MODE, SCREEN, screen) X(MIX_BLEND_MODE, OVERLAY, overlay)                         \
    X(MIX_BLEND_MODE, DARKEN, darken) X(MIX_BLEND_MODE, LIGHTEN, lighten)                         \
    X(MIX_BLEND_MODE, COLOR_DODGE, color_dodge) X(MIX_BLEND_MODE, COLOR_BURN, color_burn)         \
    X(MIX_BLEND_MODE, HARD_LIGHT, hard_light) X(MIX_BLEND_MODE, SOFT_LIGHT, soft_light)           \
    X(MIX_BLEND_MODE, DIFFERENCE, difference) X(MIX_BLEND_MODE, EXCLUSION, exclusion)             \
    X(MIX_BLEND_MODE, HUE, hue) X(MIX_BLEND_MODE, SATURATION, saturation)                         \
    X(MIX_BLEND_MODE, COLOR, color) X(MIX_BLEND_MODE, LUMINOSITY, luminosity)                     \
    X(MIX_BLEND_MODE, PLUS_DARKER, plus_darker) X(MIX_BLEND_MODE, PLUS_LIGHTER, plus_lighter)
#define AU_KWL_TEXT_DECORATION_STYLE(X)                                                           \
    X(TEXT_DECORATION_STYLE, SOLID, solid) X(TEXT_DECORATION_STYLE, DOUBLE, double_)              \
    X(TEXT_DECORATION_STYLE, DOTTED, dotted) X(TEXT_DECORATION_STYLE, DASHED, dashed)             \
    X(TEXT_DECORATION_STYLE, WAVY, wavy)
#define AU_KWL_TEXT_OVERFLOW(X) X(TEXT_OVERFLOW, CLIP, clip) X(TEXT_OVERFLOW, ELLIPSIS, ellipsis)
#define AU_KWL_USER_SELECT(X)                                                                     \
    X(USER_SELECT, AUTO, auto_) X(USER_SELECT, TEXT, text) X(USER_SELECT, NONE, none)             \
    X(USER_SELECT, CONTAIN, contain) X(USER_SELECT, ALL, all)
#define AU_KWL_BOX_ORIENT(X)                                                                      \
    X(BOX_ORIENT, HORIZONTAL, horizontal) X(BOX_ORIENT, VERTICAL, vertical)                       \
    X(BOX_ORIENT, INLINE_AXIS, inline_axis) X(BOX_ORIENT, BLOCK_AXIS, block_axis)

#define AU_KWL_FILL_RULE(X) X(FILL_RULE, NONZERO, nonzero) X(FILL_RULE, EVENODD, evenodd)
#define AU_KWL_STROKE_LINECAP(X) X(STROKE_LINECAP, BUTT, butt) X(STROKE_LINECAP, ROUND, round) X(STROKE_LINECAP, SQUARE, square)
/* miter-clip and arcs draw as miter. */
#define AU_KWL_STROKE_LINEJOIN(X)                                                                 \
    X(STROKE_LINEJOIN, MITER, miter) X(STROKE_LINEJOIN, ROUND, round) X(STROKE_LINEJOIN, BEVEL, bevel) \
    X(STROKE_LINEJOIN, MITER_CLIP, miter_clip) X(STROKE_LINEJOIN, ARCS, arcs)
#define AU_KWL_TEXT_ANCHOR(X) X(TEXT_ANCHOR, START, start) X(TEXT_ANCHOR, MIDDLE, middle) X(TEXT_ANCHOR, END, end)
#define AU_KWL_VECTOR_EFFECT(X) X(VECTOR_EFFECT, NONE, none) X(VECTOR_EFFECT, NON_SCALING_STROKE, non_scaling_stroke)
#define AU_KWL_MASK_TYPE(X) X(MASK_TYPE, LUMINANCE, luminance) X(MASK_TYPE, ALPHA, alpha)
#define AU_KWL_FONT_OPTICAL_SIZING(X) X(FONT_OPTICAL_SIZING, AUTO, auto_) X(FONT_OPTICAL_SIZING, NONE, none)
#define AU_KWL_DOMINANT_BASELINE(X)                                                               \
    X(DOMINANT_BASELINE, AUTO, auto_) X(DOMINANT_BASELINE, ALPHABETIC, alphabetic)                \
    X(DOMINANT_BASELINE, MIDDLE, middle) X(DOMINANT_BASELINE, CENTRAL, central)                   \
    X(DOMINANT_BASELINE, HANGING, hanging) X(DOMINANT_BASELINE, IDEOGRAPHIC, ideographic)         \
    X(DOMINANT_BASELINE, MATHEMATICAL, mathematical) X(DOMINANT_BASELINE, TEXT_TOP, text_top)     \
    X(DOMINANT_BASELINE, TEXT_BOTTOM, text_bottom)                                                \
    X(DOMINANT_BASELINE, TEXT_BEFORE_EDGE, text_before_edge) X(DOMINANT_BASELINE, TEXT_AFTER_EDGE, text_after_edge)

/* Every list, for code that walks all of them. */
#define AU_KWL_ALL(Y)                                                                             \
    Y(FONT_STYLE) Y(FONT_VARIANT_CAPS) Y(TEXT_ALIGN) Y(WHITE_SPACE_COLLAPSE) Y(TEXT_WRAP_MODE)    \
    Y(DIRECTION) Y(WRITING_MODE) Y(VISIBILITY) Y(LIST_STYLE_POSITION) Y(TEXT_TRANSFORM)           \
    Y(TEXT_ALIGN_LAST) Y(TEXT_WRAP_STYLE) Y(WORD_BREAK) Y(OVERFLOW_WRAP) Y(HYPHENS) Y(LINE_BREAK) \
    Y(BORDER_COLLAPSE) Y(CAPTION_SIDE) Y(EMPTY_CELLS) Y(POINTER_EVENTS) Y(CURSOR) Y(FONT_KERNING) \
    Y(TEXT_RENDERING) Y(IMAGE_RENDERING) Y(POSITION) Y(FLOAT) Y(CLEAR) Y(BOX_SIZING) Y(OVERFLOW)  \
    Y(UNICODE_BIDI) Y(TABLE_LAYOUT) Y(BORDER_STYLE) Y(OUTLINE_STYLE) Y(FLEX_DIRECTION)            \
    Y(FLEX_WRAP) Y(TRANSFORM_STYLE) Y(BACKFACE_VISIBILITY) Y(OBJECT_FIT) Y(RESIZE) Y(APPEARANCE)  \
    Y(SCROLL_BEHAVIOR) Y(ISOLATION) Y(MIX_BLEND_MODE) Y(TEXT_DECORATION_STYLE) Y(TEXT_OVERFLOW)   \
    Y(USER_SELECT) Y(BOX_ORIENT) Y(FILL_RULE) Y(STROKE_LINECAP) Y(STROKE_LINEJOIN) Y(TEXT_ANCHOR)  \
    Y(VECTOR_EFFECT) Y(MASK_TYPE) Y(FONT_OPTICAL_SIZING) Y(DOMINANT_BASELINE)

#endif
