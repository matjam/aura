/* Form control state: what a form control holds besides its attributes.
 *
 * An element that is a form control gets an AuControl the first time
 * anything needs its live state (AuElement.control, freed with the
 * element). The value is UTF-8. Until the user or script changes it (the
 * dirty value flag), the value is the default: the value attribute of an
 * input, the text of a textarea. Checkedness and option selectedness are
 * element state bits (AU_STATE_CHECKED with AU_STATE_CHECKED_SET); this
 * file only reads them.
 *
 * The editing operations (typing, deleting, moving the caret, undo) are
 * in src/page/editing.c; geometry (where the caret is, what a click hits)
 * is in src/layout/controls.c. Selection offsets here are byte offsets on
 * UTF-8 character boundaries.
 *
 * Engine rules: no OS calls, no global state. */
#ifndef AURA_DOM_CONTROL_H
#define AURA_DOM_CONTROL_H

#include "dom.h"

/* The type of an input element (HTML: the type attribute's states).
 * Unknown and missing types are text. */
typedef enum {
    AU_INPUT_TEXT,
    AU_INPUT_SEARCH,
    AU_INPUT_TEL,
    AU_INPUT_URL,
    AU_INPUT_EMAIL,
    AU_INPUT_PASSWORD,
    AU_INPUT_NUMBER,
    AU_INPUT_DATE, /* date, month, week, time, datetime-local: edited as text */
    AU_INPUT_HIDDEN,
    AU_INPUT_CHECKBOX,
    AU_INPUT_RADIO,
    AU_INPUT_FILE,
    AU_INPUT_SUBMIT,
    AU_INPUT_IMAGE,
    AU_INPUT_RESET,
    AU_INPUT_BUTTON,
    AU_INPUT_RANGE,
    AU_INPUT_COLOR
} AuInputType;

/* A file chosen in an input type=file. */
typedef struct AuControlFile {
    char *name;   /* UTF-8, no path */
    char *type;   /* MIME type ("application/octet-stream" when unknown) */
    void *data;   /* the bytes */
    size_t len;
} AuControlFile;

/* One item of a select's pop-up menu, for the embedder's menu. */
typedef struct AuPopupItem {
    const char *label;
    bool disabled;
    bool group;  /* an optgroup label (not choosable) */
    bool indent; /* an option inside a group */
} AuPopupItem;

typedef struct AuControl {
    AuBuf value;             /* the value; refreshed from the default while not dirty */
    uint32_t anchor, caret;  /* selection: anchor == caret is a caret only */
    float scroll_x, scroll_y; /* how far the text is scrolled (single-line: left; textarea: up) */
    bool dirty;              /* the dirty value flag */
    bool user_interacted;    /* for :user-invalid and the change event */
    /* Undo of the last edit: the value and selection before it. */
    bool has_undo;
    uint8_t undo_kind;       /* what the last edit was, to merge runs of typing */
    AuBuf undo;
    uint32_t undo_anchor, undo_caret;
    /* The value when the control was focused (change fires on blur when
     * the value differs). */
    char *focus_value;
    size_t focus_len;
    char *custom;            /* setCustomValidity message, or NULL */
    AuControlFile *files;
    uint32_t nfiles;
    int cursor;              /* list box: the row with the keyboard cursor (-1: none) */
    bool pressed;            /* a button held down (painted pressed) */
} AuControl;

/* The input type of el (an HTML input element). */
AuInputType au_input_type(const AuElement *el);
/* True for input types whose value is edited as text. */
bool au_input_type_is_text(AuInputType t);
/* True when el is an input edited as text or a textarea. */
bool au_ctl_is_text_field(const AuElement *el);
/* True for a single-line text field (an input edited as text). */
bool au_ctl_is_single_line(const AuElement *el);

/* The control state of el, created when missing. */
AuControl *au_ctl(AuElement *el);
/* The control state of el, or NULL when it has none yet. */
static inline AuControl *au_ctl_peek(const AuElement *el) { return (AuControl *)el->control; }
/* Frees el's control state (called when the element is freed). */
void au_ctl_free(AuElement *el);

/* The current value of a text field, range, hidden or other input, or
 * textarea (NUL-terminated, valid until the next change); *len its
 * length. The default value while the control is not dirty. */
const char *au_ctl_value(AuElement *el, size_t *len);
/* Appends the default value (value attribute, sanitized; a textarea's
 * text with CR LF made LF) to out. */
void au_ctl_default_value(const AuElement *el, AuBuf *out);
/* Sets the value as script does: sanitized for the type, dirty, the caret
 * at the end, undo dropped. */
void au_ctl_set_value(AuElement *el, const char *v, size_t len);
/* Sanitizes v for input type t into out (HTML value sanitization:
 * line breaks removed, url and email trimmed, number checked, range
 * clamped, colour lower-cased). */
void au_ctl_sanitize(AuElement *el, AuInputType t, const char *v, size_t len, AuBuf *out);
/* Resets the control: not dirty, checkedness and selectedness back to
 * the attributes, files cleared. */
void au_ctl_reset(AuElement *el);
/* Replaces the chosen files (the list is taken over; NULL, 0 clears). */
void au_ctl_set_files(AuElement *el, AuControlFile *files, uint32_t n);

/* Checkedness (checkbox, radio) and option selectedness. */
bool au_ctl_checked(const AuElement *el);
void au_ctl_set_checked(AuElement *el, bool on);

/* Number of characters (code points) in s. */
size_t au_utf8_count(const char *s, size_t len);

#endif
