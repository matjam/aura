# The Mac OS 9 shell

This document covers the Mac OS 9 application in `src/platform/macos9`:
its modules, the event loop, what the Multiversal Interfaces lack and
how Aura works around it, Open Transport, the entropy pool, the clock,
coroutines, and the Toolbox lessons learned the hard way. Read it
before changing anything under `src/platform/macos9`.
[`guest-testing.md`](guest-testing.md) says how to run the application.

## Modules

| File | Holds |
|---|---|
| `main.c` | menus and the event loop (`idle_all`, `key_down`, `release_caches`) |
| `browserwin.c` | browser windows |
| `listwin.c` | the Bookmarks, History, Downloads and Cookies windows, built from the list models ([`browser-chrome.md`](browser-chrome.md), "The list windows") |
| `prefswin.c`, `prefs_blocking.c` | the Preferences window, built from `src/browser/prefs_ui.h` ("The Preferences window" below) |
| `devwin.c` | the developer windows ([`developer-tools.md`](developer-tools.md)) |
| `storage.c` | files ([`browser-chrome.md`](browser-chrome.md), "Files Aura keeps") |
| `network.c` | the network stack's start-up, cookies and HSTS files |
| `net_ot.c`, `ot.h` | Open Transport sockets |
| `entropy.c` | the entropy pool |
| `clock_mac.c` | the wall clock |
| `coro_mac.c` | coroutines on the Thread Manager |
| `platform_mac.c` | font registration ([`fonts.md`](fonts.md)) |
| `internetconfig.c`, `contextmenu.c` | Internet Config, contextual menus ([`browser-chrome.md`](browser-chrome.md)) |
| `downloads_mac.c`, `blocking_mac.c` | download files, blocklist storage |
| `appearance.h` | Appearance Manager declarations |
| `aura.r` | resources: menus, dialogs, alerts, the SIZE resource |
| `nettest/`, `jstest/`, `sea/` | NetTest, JSTest, the installer |

The shell keeps one module per kind of window, with menus and the event
loop in `main.c` and files in `storage.c`.

- The address field is a TextEdit record placed over the field the
  engine draws.
- The scroll bars and menus are native.
- The Preferences window uses Appearance Manager controls, declared in
  `appearance.h` because the Multiversal Interfaces lack them, from
  AppearanceLib.

The application runs in a 64 MB partition; [`memory.md`](memory.md)
says what goes where. In a new Mac OS 9 program, call `au_heap_enable`
before the first allocation ([`memory.md`](memory.md)).

## What the Multiversal Interfaces lack

| Missing | Where Aura declares or finds it |
|---|---|
| Appearance Manager controls | `appearance.h`, from AppearanceLib |
| Open Transport headers | `src/platform/macos9/ot.h` declares the calls used; `net_ot.c` calls the exported `Priv` routines with its own client record, as Apple's glue did (Aura has no Apple glue library) |
| Internet Config records and Component Manager calls | `internetconfig.h` ([`browser-chrome.md`](browser-chrome.md)) |
| The Contextual Menu Manager | found in the ContextualMenu shared library at run time (`contextmenu.c`) |

Retro68's `<string.h>` pulls in the Toolbox `Strings.h`, so names like
`FInfo` collide with Toolbox types; prefix types with `Au`
([`conventions.md`](conventions.md)). newlib lacks `%lld` and `%llu`
and its `memcpy` copies byte by byte.

## Everything runs on the main task

All network work runs on the main task. Open Transport is called only
from the main task in asynchronous mode; its notifiers run at deferred
task time and only record which completion arrived, and the next pump
acts on it.

Do not call Open Transport or the engine from an MP task or a Time
Manager task: Apple's MP documentation (TN2006) lists Open Transport
among the services that are not MP-safe, and the Toolbox is not
reentrant.

## The network stack in the application

The network stack in the Mac OS 9 application is `network.c`.

- The entropy pool starts at launch, Open Transport on the first DNS
  lookup. A machine without it gets the No network page with the
  reason, and a failed start is retried after 10 seconds.
- Cookies and HSTS are saved as `Cookies` and `HSTS` in the Aura
  preferences folder 5 seconds after a change and at quit (session
  cookies are dropped).
- The disk cache is the `Cache` folder (`src/net/disk_cache.c`, limited
  by the disk cache preference).
- Develop > System Status shows the clock, entropy, Open Transport
  counters, roots, cookies and cache use in its Clock, Network and
  HTTP cache sections ([`developer-tools.md`](developer-tools.md)).
- The out-of-memory handler closes Open Transport before ExitToShell:
  with providers open at exit the guest froze.

[`networking.md`](networking.md) describes the portable stack.

## The entropy pool

The Mac OS 9 entropy pool (`src/platform/macos9/entropy.c`) gathers
loop timing jitter, user input, the mouse, clocks and free memory into
a SHA-256 pool and refuses output until it has credited 128 bits, so
the first TLS handshake waits for it (about 15 s from a cold start with
no user input, less with a seed file or input). The credit rules are in
the file's header.

A seed file, `Random Seed` in the Aura preferences folder, carries
state between runs and is rewritten at start, when the pool first
fills, and at exit.

The shell calls `au_mac_entropy_event` for every event,
`au_mac_entropy_idle` once per loop pass, and `au_ot_net_idle` with
them.

The same pool answers script's `crypto` calls (`mac_net_random`, see
[`javascript.md`](javascript.md)) and seeds the TLS library
([`tls.md`](tls.md)).

## The clock

Mac OS keeps local time, and Aura works out UTC as the clock minus the
zone offset of the Date & Time control panel (`au_mac_wall_time`,
`clock_mac.c`), so a wrong zone or daylight saving setting makes cookie
expiry, certificate dates and cache freshness wrong by hours.

To see the clock Aura uses, choose Develop > System Status and read the
Clock section: the UTC time Aura computes, script's `Date.now()` as
UTC, the Mac's local time, the zone offset and whether daylight saving
is on.

Script's `Date` uses the same clock and offset
([`javascript.md`](javascript.md), "Date and the clock").
[`guest-testing.md`](guest-testing.md) says how the test machine's
clock is set.

## Coroutines

Workers need a stack of their own. Mac OS 9 supplies it with the Thread
Manager's cooperative threads (`coro_mac.c`: a worker's thread is
stopped except while it is resumed). See [`workers.md`](workers.md).

## Keys and input

- `bw_key` does not paint: the event loop in `main.c` handles up to 8
  queued keys and then paints once (`idle_all`).
- MenuKey ignores Shift, so `key_down` maps Shift-Command-T (Reopen
  Closed Tab) and the shell maps Shift-Command-G (Find Previous)
  itself.
- [`forms.md`](forms.md) says how typed characters, the Edit menu,
  pop-up menus and file dialogs reach page fields.

## The Preferences window

`prefswin.c` builds the Preferences window from the preference
descriptions and runs it through an editing session
([`browser-chrome.md`](browser-chrome.md), "Preferences"). It holds no
values, checks or choice lists of its own.

- To move a control, change its row in the `hints` table in
  `prefswin.c`. A row gives, in window coordinates, the caption's
  position and width, the control's position and width (the height of
  a note), a radio group's second button, the unit label, the status
  line and a pop-up's menu ID. `H_AFTER_LIST` measures y from below the
  blocking list rows, which `prefs_blocking.c` makes.
- To add a setting, add its item to `prefs_ui.c` and a row to `hints`;
  an item without a row is not shown on Mac OS 9.
- The tab titles are the `'tab#'` 128 resource in `aura.r`, in the
  order of `AuPrefSection`.
- On OK the window reads every control into the session, commits it and
  calls `mac_net_prefs_changed`. On Cancel it calls
  `au_prefs_edit_cancel` and, when the blocking lists changed,
  `mac_blocking_prefs_changed`. A click on the tab position or a
  blocking check box applies it at once.
- An edit field that is not enabled (the MB field while its pop-up says
  Automatic) is deactivated and ignores clicks; a button that is not
  enabled (Make Aura the Default Browser without Internet Config) is
  hilited 255.

## The list windows

To change a list window's rows, buttons or actions, change the model
in `src/browser/lists.c` ([`browser-chrome.md`](browser-chrome.md),
"The list windows"), not `listwin.c`. `listwin.c` holds only what is
about the Toolbox:

- the window, its GWorld and the copy to the screen, with the rows
  painted by `au_listview_paint` (filled from the model with
  `au_listview_from_list`) or, for Downloads, by `au_downloads_paint`;
- the scroll bar, the push buttons made from `au_list_buttons` and
  enabled with `au_list_can` (all are disabled while the window is not
  in front), and the selection, double clicks and arrow keys;
- the `AuListHost` callbacks: DLOG 200 asks for a name, ALRT 135 shows a
  cookie's details (its lines cut to 100 characters over the four
  parameters), a pop-up menu (MENU 182) under the Site Rule button
  chooses the rule, `mac_download_reveal` reveals a download, and
  `mac_net_rules_changed` saves the rules;
- the idle check: `lw_idle` refreshes a window when `au_list_stale`
  says its data changed, and repaints Downloads at most every 250 ms.

`listwin_cookies_changed` (called after Clear Cookies) marks the Cookies
window's model stale with `au_list_touch`.

## Resources by number

| Resource | Use |
|---|---|
| DLOG 200 | asks for a name (New Folder, Rename in the Bookmarks window) |
| ALRT 135 | a cookie's or site's details in the Cookies window |
| DLOG 201 | the Find dialog |
| DLOG 202 | the About box (icon family 128) |
| ALRT 133 | offer to restore the session after a crash |
| ALRT 136 | confirms asked by the page or the browser |

## Toolbox points learned the hard way

- **Rez `MENU` enable flags.** In a Rez `MENU`, the enable flags put
  item k at bit k-1 (the menu's own flag is the separate `enabled`
  field). A separator at item 5 clears bit 4.
- **`TEGetPoint`.** Do not call `TEGetPoint` from PowerPC code; it
  crashed the guest. The caret position comes from `TextWidth` with the
  TextEdit handles locked.
- **The stack is 1 MB** (`SetApplLimit` at the start of `main`). A
  stack that overflows runs into the heap without any error. Script may
  use 512 KB of C stack before its RangeError
  (`AuJsSettings.stack_limit`), and layout called from script needs up
  to 180 KB more at its depth limit; with the earlier 256 KB a page
  whose script recursed without end froze the machine. To lower either
  limit, lower them together. `tools/smoke/recursion.html` checks it in
  the guest ([`guest-testing.md`](guest-testing.md)).
- **A dialog's colour port.** The About box's empty `dctb` gives it a
  colour port, without which `PlotIconID` draws the icon in black and
  white.
- **InternetConfigLib.** Mac OS 9.2.2 has no InternetConfigLib shared
  library (GetSharedLibrary fails with -2804 in the stock guest); call
  the component ([`browser-chrome.md`](browser-chrome.md)).
- **Open Transport at exit.** Close Open Transport before ExitToShell:
  with providers open at exit the guest froze.
- **`NewPtr` per block.** Retro68's `malloc` is one `NewPtr` per block,
  and the Memory Manager took tens of seconds per layout with the
  90,000 blocks of a large page ([`memory.md`](memory.md)).
- **Scroll bar thumbs drawn as coloured noise** on the second launch of
  Aura in a guest boot come from Enforcer, not Aura: they appear only
  on the Enforcer base, at any depth, with or without dithering.
  The Appearance theme draws thumbs through a 16x17 GWorld it keeps in
  the system heap, with its pixels in temporary memory, made the first
  time a thumb is drawn in the boot. Enforcer's temporary memory pool
  puts those pixels in a chunk of the current application (Aura) and
  frees the chunk when Aura quits; the stock Process Manager keeps
  them. The next Aura launch gets a chunk at the same address, and the
  theme's handle then names Aura's font data. That was the Enforcer of
  2026-11-14 (matjam/os9-enforcer#1). With the build installed on
  2026-11-15 the thumbs of a second and a third launch were drawn
  correctly, pixel for pixel as on the first.

## Not done

See [`known-gaps.md`](known-gaps.md).
