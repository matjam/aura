/* Forms: form owners, listed elements, select options, the entry list
 * (HTML, "Constructing the entry list") and its encodings
 * (application/x-www-form-urlencoded, multipart/form-data, text/plain),
 * and constraint validation. Portable; no OS calls, no global state.
 * Submission itself (navigation, events) is in forms.c. */
#ifndef AURA_PAGE_FORMDATA_H
#define AURA_PAGE_FORMDATA_H

#include "../dom/control.h"

/* ---- Elements ---- */
/* The form owner of a listed element: the form its form attribute names
 * (by id, in its tree), else the nearest form ancestor; NULL when none. */
AuElement *au_form_owner(AuElement *el);
/* True for listed elements (button, fieldset, input, object, output,
 * select, textarea). */
bool au_form_is_listed(const AuElement *el);
/* True for submittable elements (button, input, select, textarea). */
bool au_form_is_submittable(const AuElement *el);
/* A control's disabled state: its disabled attribute, or a disabled
 * fieldset ancestor (outside that fieldset's first legend), or for an
 * option, its own or its optgroup's. */
bool au_form_disabled(const AuElement *el);
/* The listed elements whose owner is form, in tree order, into a new
 * array (*n of them; au_free it). */
AuElement **au_form_elements(AuElement *form, size_t *n);
/* A submit button: button with type submit (the default), input type
 * submit or image. */
bool au_form_is_submit_button(const AuElement *el);
/* The form's default button: its first submit button in tree order, or
 * NULL. */
AuElement *au_form_default_button(AuElement *form);
/* The elements of a radio group (same name, same owner, same tree) with
 * el, into a new array. */
AuElement **au_form_radio_group(AuElement *el, size_t *n);

/* ---- Select ---- */
/* The list of options of select (its option children and the options in
 * its optgroup children), into a new array. With groups, optgroup
 * elements are included too, in order (list box rows). */
AuElement **au_select_options(AuElement *sel, bool groups, size_t *n);
/* True when select shows as a list box (multiple, or size > 1). */
bool au_select_is_listbox(const AuElement *sel);
/* The selected index of a select among its options (not groups), as
 * script sees it: the first selected option; for a drop-down without one
 * the first enabled option; -1 when none. */
int au_select_index(AuElement *sel);
/* Selects option index i (among options) alone, as a user choice does. */
void au_select_set_index(AuElement *sel, int i);
/* The value of an option: its value attribute, else its text with white
 * space collapsed. Appended to out. */
void au_option_value(const AuElement *opt, AuBuf *out);
/* The label of an option (label attribute, else text), collapsed. */
void au_option_label(const AuElement *opt, AuBuf *out);

/* ---- The entry list ---- */
typedef struct AuFormEntry {
    char *name;
    char *value; /* a string value, or a file's bytes */
    size_t value_len;
    bool file;
    char *filename; /* file entries */
    char *type;
} AuFormEntry;

typedef struct AuFormEntries {
    AuFormEntry *e;
    size_t n, cap;
} AuFormEntries;

void au_form_entries_init(AuFormEntries *l);
void au_form_entries_free(AuFormEntries *l);
void au_form_entries_add(AuFormEntries *l, const char *name, const char *value, size_t len);
void au_form_entries_add_file(AuFormEntries *l, const char *name, const char *filename, const char *type,
                              const void *data, size_t len);
/* Builds the entry list of form with submitter (NULL: none; an image
 * button submitter adds name.x and name.y at image_x, image_y). */
void au_form_entries_build(AuElement *form, AuElement *submitter, int image_x, int image_y, AuFormEntries *out);

/* The encodings. Line breaks in names and values become CR LF. */
void au_form_urlencode(const AuFormEntries *l, AuBuf *out);
/* application/x-www-form-urlencoded byte serializer of one string. */
void au_form_urlencode_string(const char *s, size_t n, AuBuf *out);
void au_form_multipart(const AuFormEntries *l, const char *boundary, AuBuf *out);
void au_form_text_plain(const AuFormEntries *l, AuBuf *out);

/* ---- Validation ---- */
/* Why a control is invalid (AuValidity bits); 0 when valid or barred
 * from validation. pattern_test, when given, tests the whole value
 * against a pattern attribute (the script engine's RegExp); without it
 * patterns are not checked. */
enum {
    AU_VALID_MISSING = 1,
    AU_VALID_TYPE = 2,
    AU_VALID_PATTERN = 4,
    AU_VALID_TOO_LONG = 8,
    AU_VALID_TOO_SHORT = 16,
    AU_VALID_UNDERFLOW = 32,
    AU_VALID_OVERFLOW = 64,
    AU_VALID_STEP = 128,
    AU_VALID_BAD_INPUT = 256,
    AU_VALID_CUSTOM = 512
};
typedef int (*AuPatternTest)(void *ctx, const char *pattern, const char *value); /* 1 match, 0 no, -1 bad pattern */
bool au_form_will_validate(AuElement *el);
unsigned au_form_validity(AuElement *el, AuPatternTest test, void *ctx);
/* An English message for the first reason in bits. */
const char *au_form_validity_message(AuElement *el, unsigned bits);

#endif
