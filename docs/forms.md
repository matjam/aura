# Forms

This document covers form controls: their state in C, painting,
editing, input, submission, the script API, the Mac OS 9 side, and what
a keystroke costs. Read it before changing a control, text editing,
focus or submission, or when typing in a page is slow.

Form controls keep their state in C, so typing, clicks and submission
work with script off, and script reads and writes the same state.

## To drive forms on the host

Use `aura-host chrome` with `--do` actions, run in order after the page
loads:

| Action | Does |
|---|---|
| `clickname:NAME` | clicks the element with that name attribute |
| `click:X,Y` | clicks a point |
| `type:TEXT` | types text |
| `key:[cmd-][shift-][opt-]NAME` | presses a key |
| `keys:TEXT` | types TEXT and prints the cost of each key |
| `edit:copy\|cut\|paste:TEXT` | runs an Edit command |
| `wait:MS` | waits at least MS, then until loads finish |
| `pause:MS` | waits exactly MS while loads run (with `--loading`, to see a page part loaded, such as text in a web font's block period, [`fonts.md`](fonts.md#block-periods-and-font-display)) |
| `shot:FILE.png` | writes a screenshot |
| `console` | prints the console |

For example, `aura-host chrome --net --tab
https://html.duckduckgo.com/html/ --do clickname:q --do "type:mac os 9"
--do key:return --do wait:3000 -o run/ddg.png`.

## To measure a keystroke

- On the host, run `aura-host chrome --tab PAGE --do clickname:NAME
  --do keys:TEXT`: for each key it prints the time in the event, the
  idle pass and painting, and the counts from `au_docview_stats` (style
  passes, elements styled, tree builds, layouts) and the pixels
  invalidated.
- In the guest, click a text field of the page and choose Develop > Run
  Timing Test: it types 16 keys, and the "Timing test" section of the
  Performance window shows milliseconds per key with the restyle and
  layout counts ([`developer-tools.md`](developer-tools.md)).
- `test_keystroke_cost` in `tests/forms` asserts zero of each on a page
  with `:has()` rules, with and without script listeners.

## Where things are

| File | Holds |
|---|---|
| `src/dom/control.c` | control state |
| `src/layout/controls.c` | painting, points to offsets |
| `src/page/editing.c` | the portable editing model |
| `src/page/forms.c` | input: focus, edits, activation |
| `src/page/formdata.c` | entry lists, encodings, validity |
| `src/js/natives_forms.c`, `prelude_html.js` | the script API |

## Control state

`src/dom/control.c` gives each input, textarea, select and option an
`AuControl` on first use (`au_ctl`): the UTF-8 value and its dirty
flag, the selection as byte offsets (anchor and caret), scroll offsets,
one undo step, the value at focus (for change), the custom validity
message, chosen files, the list box cursor and the pressed flag.
Checkedness is the element state `AU_STATE_CHECKED` with
`AU_STATE_CHECKED_SET`. Layout reads the value at paint time, so typing
repaints without a box tree rebuild.

## Painting

`src/layout/controls.c` paints text fields and textareas (placeholder,
password bullets, selection, a one-pixel caret, soft wrapping and
scrolling) and maps points to offsets (`au_layout_field_offset_at`,
`au_layout_field_caret_rect`). A textarea is a replaced box; its
children are not laid out.

The placeholder takes its colour from the field's `::placeholder`
style. The cascade computes that style for every `input` and `textarea`
that a `::placeholder` rule matches (`attach_pseudos` in
`src/css/cascade.c`), so `color: transparent` hides the placeholder as
in Chrome. Without a page rule the colour is `#757575` from the UA
sheet.

A text input's natural width (`replaced_intrinsic_size` in
`src/layout/layout.c`) is `size` + 1 times the width of "x", rounded, as
Chrome measures it on the Mac: 147 pixels of content at 13.33 pixels
for the default `size` of 20 in Arial, Helvetica, Times and SF alike.
The OS/2 average character width gave 115 to 172 pixels by face, and a
wide field pushed the search button of Python's documentation past a
400-pixel window.

A single-line select is its widest option plus 19 pixels at 13.33
pixels, as Chrome's is on the Mac: 4 pixels of left padding (the UA
sheet), a 13-pixel arrow (`l_select_arrow_w`, 0.975 of the font size)
and the border. Before 2026-11-17 it was 26 pixels. Aura's controls
keep `system-ui` where Chrome on the Mac uses Arial, so a select or
field is still a little wider than Chrome's: Python's documentation is
5 pixels wider than a 400-pixel window.

## Editing

`src/page/editing.c` is the portable model: insertion with maxlength
and single-line rules, deletion by character and word, movement, word
and line boundaries, and undo (Undo twice redoes).

## Input

`src/page/forms.c` turns keys and mouse events into focus changes,
edits and activation:

- click and Tab focus (positive tabindex first, then tree order);
- autofocus at the end of parsing;
- the caret blink (`au_docview_blink`, which repaints only the caret);
- checkbox, radio group, select, list box, range, file, details and
  label behaviour;
- implicit submission;
- the Edit commands (`au_docview_edit`).

With the JavaScript runtime running, a click's activation runs in the
bindings and reaches C through `N.submitForm`; without it
`au_forms_activate` does the same in C.

Inert elements ([`layout.md`](layout.md), "Inert content") are not
focusable (`focusable` in `forms.c`, `isFocusable` in the bindings), so
Tab cycles inside an open modal dialog and a click or `focus()` cannot
move the focus out of it. A press where only inert content is under
the pointer leaves the focus where it is.

## contenteditable

To edit an element with `contenteditable`, Aura keeps the caret in the
script `Selection` (`range.js`) and does the editing in script, in the
"contenteditable editing" part of `prelude_obs.js`:

- **Focus.** An editing host (editable, its parent not) is focusable.
  Focusing it (a press, `focus()`, Tab) keeps a `Selection` already in
  it and otherwise puts the caret at the start of its text.
- **A press** in an editing host asks layout for the text position under
  the pointer (`au_layout_text_pos_at` in `au_forms_mouse`) and calls
  `P.editClick`, which collapses the `Selection` there, or at the end of
  the host's text where no text is under the pointer.
- **Keys.** A key whose keydown and keypress nobody cancelled reaches
  `P.editKey` (`edit_key` in `forms.c`, after text fields): a character
  is inserted, Backspace and Delete remove a character (a pair of
  surrogates together, or a `<br>`), Enter inserts a `<br>` (two at the
  end of the host, so the new line shows), Left and Right move by a
  character (Shift extends), Home and End go to the start and end of the
  host's text, and Command-A selects its contents. Each change fires
  `beforeinput` (cancelable: a cancelled one changes nothing) and
  `input` with the `inputType` and `data` browsers give.
- **The caret.** `N.editCaret` names the `Selection`'s live range while
  the focus is in an editing host and the `Selection` is collapsed in
  it, so DOM changes move the caret with the range. The view draws it
  over the painted page (`au_dv_edit_caret_rect`, `docview.c` paint):
  layout's caret rectangle for a collapsed text range, or the start of
  the element's box, one line tall, where no text is laid out (Slate's
  zero-width text). It blinks with a field's caret, and
  `au_docview_editing` is true, so Space and the arrows edit rather
  than scroll the page.
- **Rich editors.** `InputEvent.getTargetRanges` is not defined, so
  Slate (Discord's message box) takes its path for browsers without
  `beforeinput`: it cancels keydown and the keypress React turns into
  `onBeforeInput`, edits its own model, and sets the `Selection`, which
  moves the caret. `test_contenteditable` in `tests/forms/test_forms.c`
  covers the press, typing, cancelled events, Backspace, the arrows,
  Enter and the caret.

## Dialogs and popovers

Dialogs and popovers live in the bindings (`prelude_html.js`, "the top
layer"); C keeps the top layer and the states ([`layout.md`](layout.md),
"The top layer").

- `showModal()` puts the dialog in the top layer, closes the auto
  popovers that are not its ancestors and focuses the dialog's
  autofocus element or first focusable element. `close()` takes it out
  and gives the focus back to the element that had it.
- To close by key, press Escape: `au_docview_key` calls
  `au_docscript_close_request` when the page did not cancel keydown,
  before a field's own Escape. The topmost modal dialog gets `cancel`
  and closes unless a listener cancels it; an auto popover on top hides
  instead.
- Popovers follow the HTML Standard: `showPopover`, `hidePopover`,
  `togglePopover`, `beforetoggle` (cancelable when showing), `toggle`
  in a task with the states coalesced, `auto` (and `hint`) popovers
  that close the auto popovers that are not their ancestors, `manual`
  ones that stay, and `popovertarget` buttons with
  `popovertargetaction`. A page with a `popovertarget` attribute starts
  the script runtime at the end of parsing.
- Light dismiss: `au_docview_mouse` calls `au_docscript_light_dismiss`
  at the press and at the release while a popover shows; when both land
  in the same popover (or its button), or outside every one, the auto
  popovers above it hide.

To check a modal dialog on a live page, use a fresh profile folder (the
default one may have the page's banner hidden) and log the page's
clicks. On the Wikipedia portal (checked 2026-11-15) the "I already
donated" link opens the thank-you dialog with `showModal()`:

```bash
aura-host chrome -w 1024 -h 768 --net --profile run/wp-profile \
  --tab https://www.wikipedia.org/ --do wait:5000 \
  --do "js:window.L=[];document.addEventListener('click',function(e){L.push(e.target.tagName)},true);1" \
  --do click:861,197 --do wait:1500 --do click:371,209 --do click:150,600 \
  --do "js:L.join(' ')+' '+location.href" --do key:escape \
  --do "js:document.querySelector('dialog').open"
```

The clicks outside the dialog go to the dialog (its backdrop), not to
the links under it, and Escape closes it. The link's place depends on
the banner; find it with a `js:` action that prints its
`getBoundingClientRect()` (the page starts 57 pixels down the window).

## Submission

`src/page/formdata.c` builds the entry list (`au_form_entries_build`)
and encodes it (urlencoded, multipart with file parts, text/plain), and
checks validity (`au_form_validity`; pattern goes through the script
RegExp when the runtime runs).

`form_submit` in `forms.c` applies the formaction, formmethod,
formenctype, formtarget and formnovalidate attributes, asks before an
https page sends a form over http, and passes an `AuFormSubmission` to
`AuDocViewCallbacks.submit`.

`page_doc.c` sends GET through `open_url` and POST to the browser
(`submit_post`), which adds a history entry and calls
`au_page_navigate_post`; the fetcher sends the body. Reloading a POST
result asks first (`AuBrowserHost.confirm`); a history entry of a POST
result loads with GET.

## Script

`src/js/natives_forms.c` exposes the control state (`N.ctlValue`,
`N.ctlSel`, `N.ctlFiles` and so on), converting selection offsets
between UTF-16 and UTF-8. `prelude_html.js` builds the DOM API on them
(value, selectionStart, setRangeText, files, validity, requestSubmit)
and the legacy named access (`document.formName`, `form.controlName`,
`form[0]`, RadioNodeList) through a proxy between `Node.prototype` and
`EventTarget.prototype`.

Form-associated custom elements (`static formAssociated = true`, with
`attachInternals()`) live in the bindings (`prelude_obs.js`,
"ElementInternals"): `setFormValue` gives the form a string or the
entries of a FormData; `new FormData(form)` reads them, and a
submission passes the string entries to C as `N.submitForm`'s third
argument (`au_forms_script_submit` appends them after the controls'
entries). `setValidity` joins `checkValidity`, `reportValidity` and
interactive validation; `form.reset()` calls `formResetCallback`. Their
file values are not submitted, and `:state()` does not match
([`known-gaps.md`](known-gaps.md)).

Events go in the standard order: keydown, beforeinput, input, keyup,
change at blur or implicit submission, submit. Enter also fires
keypress (code 13) before implicit submission.

## The Mac OS 9 shell

- `browserwin.c` maps typed Mac Roman characters to Unicode and passes
  every key to the page when the address field does not have the focus.
- `main.c` routes Command-arrows and Command-Delete to the window
  instead of MenuKey.
- The Edit menu acts on a focused page field (`page_field_edit`, TEXT
  scrap in Mac Roman).
- Selects use `PopUpMenuSelect` (`host_popup`), file inputs
  `StandardGetFile`, confirms ALRT 136, and the I-beam shows over text
  fields (`au_bw_text_field_at`).
- `bw_key` does not paint: the event loop in `main.c` handles up to 8
  queued keys and then paints once (`idle_all`).

## What a keystroke costs

A character typed into, or deleted from, a text field that stays
non-empty restyles nothing, builds no box tree, runs no layout and
invalidates the field's rectangle only. `edited` in `forms.c` restyles
only when `field_style_state` changes: the field becomes empty or stops
being empty (`:placeholder-shown`), or its validity bits change.

Measured in the guest on a 400-paragraph page with `:has()` rules:

| | Per key |
|---|---|
| Before (a restyle of every element and a layout per key) | 61 to 124 ms |
| After | 4.5 ms |

### What script does per key still costs

- A change in a subtree that is not in the document marks nothing dirty
  (`h_dom_changed` in `script.c`).
- Attributes the box tree never reads (`aria-*`, `data-*`, `role`,
  `title`, `tabindex`, a hidden input's `value`) restyle without a box
  tree rebuild (`au_dv_dom_changed`).
- While a key is handled, `au_dv_turn_end` does not invalidate the page
  for what listeners changed: `au_docview_key` updates at its end and
  invalidates the page only if the update changed geometry or raised a
  paint, so a restyle that computes the same values repaints the field
  only (`test_keystroke_cost`, "attribute writers").

On live Wikipedia (`aura-host chrome -w 1440 -h 900 --net --tab
https://en.wikipedia.org/wiki/Mac_OS_9 --do wait:4000 --do
clickname:search --do wait:3000 --do keys:macint`) the box tree build
on a key came from the suggestion list's thumbnails: an image whose
size became known made layout build the tree again and the view
invalidate the page, at the next update, which a key runs. An image
change now keeps the tree (see [`layout.md`](layout.md)), and a key
there restyles 2 elements, builds no tree, runs no layout and
invalidates the field (439 x 41 pixels).

When the suggestion list changes, its DOM insertions now rebuild only
the list's subtree ([`layout.md`](layout.md), "A DOM change"): six keys
with waits cost 54 subtree rebuilds and no tree build
([`performance.md`](performance.md), "DOM changes"). What remains is
the page's own work: keys typed while the page still loads its search
module pay for that loading.

## Tests

- `tests/forms`: editing, encodings, focus, submission, validation,
  event order, and a modal dialog and popovers driven by clicks and
  keys (`test_modal_dialog`).
- `tests/js/forms.js`: bindings and named access.
- `tests/js/top_layer.js`: dialogs, popovers and the `inert`
  attribute.
- The forms part of `tests/browser/test_net_page.c`: GET and POST over
  the fake transport.
- `tests/page/samples/qa-forms.html`: every control, and a Try it form
  that shows what it sent.

## Not done

See [`known-gaps.md`](known-gaps.md#forms).
