/* The text editing model of form fields: insertion, deletion, caret and
 * selection movement by character and word, selection, and undo of the
 * last edit, on an AuControl (src/dom/control.h). Movements that need
 * geometry (line ends, up and down in a textarea) are computed by
 * src/layout/controls.c and applied with au_edit_move_to.
 *
 * Offsets are bytes on UTF-8 character boundaries. Callers bring the
 * value up to date (au_ctl_value) before an edit; every edit sets the
 * dirty value flag. Engine rules: no OS calls, no global state. */
#ifndef AURA_PAGE_EDITING_H
#define AURA_PAGE_EDITING_H

#include "../dom/control.h"

/* Kinds of edit, for undo: runs of typing (and of deleting) merge into one
 * undo step. */
enum { AU_EDIT_OTHER = 0, AU_EDIT_TYPING, AU_EDIT_DELETING };

/* The selection as start <= end. */
void au_edit_range(const AuControl *c, uint32_t *start, uint32_t *end);
bool au_edit_has_selection(const AuControl *c);

/* Character and word boundaries in s (len bytes) before and after off. A
 * character is a code point with its combining marks. A word movement
 * backward skips separators then word characters (Option-Left); forward
 * it skips separators then a word (Option-Right ends after the word). */
uint32_t au_edit_prev_char(const char *s, size_t len, uint32_t off);
uint32_t au_edit_next_char(const char *s, size_t len, uint32_t off);
uint32_t au_edit_prev_word(const char *s, size_t len, uint32_t off);
uint32_t au_edit_next_word(const char *s, size_t len, uint32_t off);
/* The word (or run of separators) around off, for a double click. */
void au_edit_word_at(const char *s, size_t len, uint32_t off, uint32_t *start, uint32_t *end);
/* The line (between line breaks) around off, for a triple click in a
 * textarea. */
void au_edit_line_at(const char *s, size_t len, uint32_t off, uint32_t *start, uint32_t *end);

/* Replaces the selection with s. max_chars (-1: no limit) is maxlength:
 * the inserted text is cut so the value stays within it. single_line
 * turns line breaks into spaces (a paste into an input). Returns false
 * when nothing changed. */
bool au_edit_insert(AuControl *c, const char *s, size_t n, long max_chars, bool single_line, int kind);
/* Deletes the selection, or else the character (word with by_word)
 * before the caret (dir < 0, Backspace) or after it (dir > 0, Delete). */
bool au_edit_delete(AuControl *c, int dir, bool by_word);
/* Deletes from the caret to offset to (Command-Backspace to the line
 * start), or the selection when there is one. */
bool au_edit_delete_to(AuControl *c, uint32_t to);
/* Moves the caret to off; extend keeps the anchor (Shift). */
void au_edit_move_to(AuControl *c, uint32_t off, bool extend);
/* Moves by a character or word (dir -1 or 1). Without extend, a
 * selection collapses to its start or end first. */
void au_edit_move(AuControl *c, int dir, bool by_word, bool extend);
void au_edit_select(AuControl *c, uint32_t anchor, uint32_t caret);
void au_edit_select_all(AuControl *c);
/* Appends the selected text to out. */
void au_edit_selected_text(const AuControl *c, AuBuf *out);
/* Undoes the last edit (a second undo redoes it, as on the Mac). Returns
 * false when there is nothing to undo. */
bool au_edit_undo(AuControl *c);

#endif
