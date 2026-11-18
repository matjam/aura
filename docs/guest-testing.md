# Guest testing

This document covers Aura's test machine: a QEMU mac99 guest running
Mac OS 9.2.2, with outbound network access. It says how to start it,
copy a build in, drive the screen from the host, which base image to
test on and how the guest's clock is set. Read it before running anything in the guest.
[`building.md`](building.md) says how to install QEMU, the disk images
and hfsutils.

Runtime files go in `run/` of the main checkout (set `AURA_RUN` to use
another folder, for example to run a second guest beside the first);
`tools/guest-env.sh` holds the settings.

## Quick reference

| To | Run |
|---|---|
| start the guest | `tools/guest-run.sh start` |
| start it in a window, to use by hand | `WINDOW=1 tools/guest-run.sh start` |
| see which base runs | `tools/guest-run.sh status` |
| copy a build in | `tools/guest-push.sh -n AuraTest build/ppc/dist/Aura/*` |
| launch Aura | `tools/guest-qmp.py open AuraTest Aura` |
| take a screenshot | `tools/guest-qmp.py screenshot run/NAME.png` |
| press a key | `tools/guest-qmp.py key cmd-q` |
| type text | `tools/guest-qmp.py type TEXT` |
| click, drag | `tools/guest-qmp.py click X Y`, `drag X1 Y1 X2 Y2` |
| print the current application | `tools/guest-qmp.py front-app` |
| print the front window's title | `tools/guest-qmp.py front-window` |
| wait for an application or a window | `tools/guest-qmp.py wait-app APP [SECONDS]`, `wait-window APP TITLE [SECONDS]` |
| check a build end to end | `tools/smoke.sh` |
| time page loads (the page benchmark) | `tools/bench.sh` ([`performance.md`](performance.md), "To benchmark pages") |
| stop the guest | `tools/guest-run.sh stop` |

## Which base to test on

Test on the stock base: Mac OS 9.2.2 as installed, with virtual memory
off, which is what most users run. Aura is not tested under Enforcer
since 2026-11-18 ("The Enforcer images (retired)" below). Before that,
Enforcer 0.1.0 and 0.2.0 reported nothing caught for Aura 0.4.1, and
one Enforcer bug (scroll bar thumbs drawn as noise on a second launch)
is recorded in [`mac-os-9-shell.md`](mac-os-9-shell.md).

## To start and stop the guest

To start the guest, run `tools/guest-run.sh start`. QEMU runs headless
in the background. Mac OS takes about a minute to boot.

The guest boots `disks/os9-novm.qcow2` (`BASE=stock`, the only base):
Mac OS 9.2.2 with virtual memory off.

- **Overlays.** The guest boots an overlay on the base image
  (`run/os9-overlay-BASE.qcow2`), so the base is never written. Disk
  writes are discarded when QEMU exits; set `PERSIST=1` to keep them in
  the overlay.
- **Stale overlays.** An overlay is valid only for the base image it
  was made on, and booting it on a changed image damages the guest's
  disk. `guest-run.sh` records the base image's modification time and
  size beside the overlay (`os9-overlay-BASE.qcow2.base`), and when
  they differ at `start` or `reset` it renames the overlay
  `os9-overlay-BASE.stale.qcow2` and makes a new one.
- **One guest runs per run folder**; `status` names the running base.
- **Screen size.** `GEOMETRY` sets the largest screen mode the guest
  offers (default `1024x768x32`; for example `GEOMETRY=1440x1050x32`);
  Mac OS starts at 640x480 or the last mode chosen, and the Control
  Strip's resolution module switches to it.

To stop the guest, run `tools/guest-run.sh stop`, or choose Special >
Shut Down in the guest.

## To copy the application in

Build it with `./build.sh ppc` and run `tools/guest-push.sh -n AuraTest
build/ppc/dist/Aura/*`, which puts the application and its `Fonts`
folder side by side at the top of the disc (push
`build/ppc/src/platform/macos9/Aura.bin` alone to test without the
bundled fonts).

- A folder argument is copied with its subfolders.
- Fonts get type `tfil` and creator `AURA`; licences and READMEs become
  SimpleText documents.
- `.html`, `.css`, `.js` and `.idx` files are copied byte for byte
  (hcopy's text mode would translate UTF-8 as Latin-1).
- A `.hqx` file is copied byte for byte as a StuffIt Expander text
  document, for the guest to decode.
- A `.bin` file is decoded as MacBinary.
- HFS names are at most 31 characters.

The files arrive as a CD named AuraTest on the guest's desktop. The
previous disc is ejected first through the Finder; the script types
nothing until the guest's current application (`CurApName`) is the
Finder, and fails if an application on the disc is still open, so quit
Aura before pushing a new build.

To start Aura with clean state, delete the Aura folder in the guest's
Preferences folder ([`browser-chrome.md`](browser-chrome.md), "Files
Aura keeps").

## To launch Aura and drive the screen

To launch it, run `tools/guest-qmp.py open AuraTest Aura`. It exits 0
when Aura is the current application and 1 with the reason when a step
did not happen (the disc is not mounted, the disc or the item could not
be selected, the application did not come up within `OPEN_TIMEOUT`
seconds, 120 by default). A third argument names the application to
wait for when it is not the item's name (a document, say); `-` skips
the wait.

To take a screenshot, run `tools/guest-qmp.py screenshot run/NAME.png`.
To type, use `tools/guest-qmp.py key cmd-q` (any character works as the
key, as in `'cmd-['`) or `type TEXT`; to click or drag, use `click X Y`
or `drag X1 Y1 X2 Y2` (guest screen pixels).

`front-app` prints the current application, `front-window` the title of
its front window (for Aura, the page's title) and `volumes` the mounted
volumes. To wait in a script, use `wait-app APP [SECONDS]` and
`wait-window APP TITLE [SECONDS]`; both exit 1 on a timeout.

**Menus.** The Develop menu is in the menu bar (its title is at 298,10
on a 640x480 screen): click the title, then click the item; the menu
stays open between the two clicks. While Aura is busy (a long layout),
the click is lost; wait and try again. [`developer-tools.md`](developer-tools.md)
describes the items and the windows the menu opens.

To choose an item of the Develop > Tests submenu, click the Develop
title, then run `guest-qmp.py move 300 142` (the Tests item), `move 420
142`, and click the item. A click on the Tests item itself closes the
menu.

**Dragging in the Finder.** The Finder ignores `guest-qmp.py drag` (it
moves in one step). To drag a file in the Finder (onto the Aura icon,
or a copy to the hard disk), hold the button for most of a second and
move in about ten steps of 0.15 s each, with the QMP class from
`tools/guest-qmp.py`.

**Screen depth.** To change the screen depth, click the Monitor
BitDepth module in the guest's Control Strip (the seventh icon, at
197,466 on a 640x480 screen) and pick 256 Colors, Thousands or
Millions.

**Coordinates on a 640x480 screen**

| Thing | At |
|---|---|
| Develop menu title | 298,10 |
| Develop menu items, by y: Page Source, Console, Network | 28, 44, 60 |
| Develop menu items, by y: Performance, Run Timing Test | 82, 98 |
| Develop menu items, by y: System Status, Tests, Report a Problem | 120, 142, 164 |
| Special menu title | 246,10 |
| Special > Shut Down | 270,136 |
| Monitor BitDepth module (Control Strip, seventh icon) | 197,466 |
| Aura's content area starts | 20,50 |

## To check a build in the guest after a merge (the smoke test)

Run `./build.sh ppc` and then `tools/smoke.sh`.

- It boots a stock guest in its own run folder (`run-smoke` in the main
  checkout, or `AURA_RUN`), pushes `build/ppc/dist/Aura`, launches
  Aura, and opens a local sample page, `tools/smoke/recursion.html` (a
  script that recurses without end and must get a RangeError) and
  https://news.ycombinator.com/.
- It waits for each page's window title, takes a screenshot of each
  (`smoke-local.png`, `smoke-recursion.png`, `smoke-live.png`), checks
  that Aura is still the current application, quits it and stops the
  guest.
- It exits 0 when every step passed and 1 with the failed step
  otherwise.
- `SMOKE_LIVE=0` skips the live page; `SMOKE_KEEP=1` leaves the guest
  running.
- It takes about four minutes.

## To change how `open` or `guest-eject` drive the Finder

Keep to this rule: check each step before sending the next key.

Typing a name in the Finder selects the nearest name when none matches,
so "AuraTest" typed before the pushed disc shows on the desktop selects
Browse the Internet, and Command-O then launches it (Internet Setup
Assistant, Internet Explorer and Outlook Express were opened this way).

- `open_item` waits for the disc in the volume queue, types the name,
  and confirms the selection with Get Info (Command-I opens a window
  titled "NAME Info" and opens nothing; `select_checked`), closes that
  window, opens the disc, waits until the Finder's front window is the
  disc's, and confirms the item the same way before Command-O.
- The window list (`WindowList`, 0x9D6), `CurApName` and the volume
  queue (0x358) are read from guest memory with the guest stopped (QMP
  `stop`, then `cont`): they belong to whichever process is switched
  in, and about three reads in four land in a moment when the monitor
  cannot read Mac OS's memory at all, so every caller polls.
- Command-Shift-Up makes the desktop active and selects the startup
  disk; do not send it between confirming a selection and Command-O.

## To open a local page in the guest

Push it with the application (for example `tools/guest-push.sh -n
AuraTest Aura.bin tests/page/samples/*`) and choose File > Open File,
drop it on the Aura icon, or type its path (`AuraTest:wiki.html`) in
the address field.

To send a GURL Apple event, run `open location "URL"` in Script Editor.

## To compare the window with the host rendering

Render the scene at the window's size (the title shows it) and compare
the content area, which starts at 20,50 on the guest screen:

```bash
build/host/src/platform/host/aura-host scene -w 584 -h 390 -o run/host.png
tools/png-compare.py run/guest.png 20 50 run/host.png run/diff.png
```

## Other test programs for the guest

| Program | Tests | Document |
|---|---|---|
| `NetTest.bin` | the network stack and TLS timings | [`networking.md`](networking.md) |
| `JSTest.bin` | the JavaScript engine self-test and benchmark | [`javascript.md`](javascript.md) |
| the installer archive | unpacking with StuffIt Expander | [`ci-and-releases.md`](ci-and-releases.md) |

To test the crash-recovery prompt, press Command-Option-M in Aura (the
out-of-memory test) and launch Aura again
([`browser-chrome.md`](browser-chrome.md)).

## The guest's clock

On the test machine both base images are set to Cupertino with daylight
saving on (UTC-7, measured on 2026-11-15). QEMU would start the guest's
hardware clock at UTC, which Mac OS takes for local time (Aura's UTC
was then 7 hours ahead, and a cookie with a 30-minute `Expires` was
dropped as expired).

`tools/guest-run.sh` therefore passes `-rtc base=` the current UTC plus
`AURA_GUEST_UTC_OFFSET` (`tools/guest-env.sh`, -25200 for both images),
so the guest's menu bar clock shows Cupertino daylight time and Aura's
UTC matches the host's `date -u` to the second.

- If you change an image's zone or daylight saving setting, change that
  value (or set it in the environment for a `PERSIST=1` overlay whose
  zone differs from its base image's).
- A guest started before this change keeps its old clock until it is
  restarted.
- To see the clock Aura uses, choose Develop > System Status and read
  its Clock section ([`mac-os-9-shell.md`](mac-os-9-shell.md), "The
  clock").

## The Enforcer images (retired)

Until 2026-11-18 the default base was `os9-vmoff.qcow2`, the same
system with Enforcer 0.2.0+bd0d2bc installed. The owner retired it: a
freeze of Aura on GitHub in the owner's guest may have come from
Enforcer, and most users run without it. `disks/os9-vmoff*.qcow2` stay
on disk but no script boots them; a run folder's
`os9-overlay-enforcer.qcow2` is set aside as
`os9-overlay-enforcer.retired.qcow2` (its files can still be copied out
with `qemu-img convert` and hfsutils).
