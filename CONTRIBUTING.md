# Contributing to Aura

Please contribute. The most useful thing you can do is run Aura on a
real Mac and say what happened, because so far it has run only in an
emulator (see the AI use disclosure in `README.md`).

## Tell me how it went

You do not need to find a bug to help. Open an issue and say:

- Which Mac (model, processor, RAM), which Mac OS version, and whether
  it is real hardware or an emulator.
- How Aura is connected to the network (Ethernet, AirPort, a proxy).
- What you saw. For example:
  - Which sites loaded and looked right, and which did not?
  - How long does a page you care about take to load? Is scrolling and
    typing fast enough to use?
  - How much memory did you give Aura in Get Info, and did it run out?
  - Does anything crash, freeze the Mac, or stop working after a while?

To send measurements, load a page, choose Develop > Performance, click
Copy Report and paste the text into the issue. It lists the time spent
in script, style, layout and painting, and memory use.

"Wikipedia loads in 20 seconds on my 867 MHz Quicksilver with 768 MB"
is a useful report.

## Report a site that does not work

Most bugs will be of this kind. Open an issue with:

- The address of the page.
- What you expected and what you saw. A screenshot helps most.
- The Aura version. It is in the About box, for example "Aura 0.3.0".
- What these show, if you can:
  - Develop > Console: copy any errors.
  - Develop > Network: open it, reload the page, and click Copy All. It
    lists every request, including any that failed or were blocked.
  - Whether the page works with blocking turned off for the site (the
    page's contextual menu) and with JavaScript turned off in
    Preferences. That tells which part is involved.

A page that needs you to sign in is hard for me to test. If the problem
shows on a public page too, give that one.

## Report a crash or a freeze

1. Open an issue.
2. Say what you did, and the address of the page if a page was open.
   Steps that make it happen again are worth more than anything else.
3. Add what you can of:
   - The Aura version, the Mac and the Mac OS version.
   - How much memory Aura has in Get Info, and whether virtual memory
     is on.
   - The error number or message, or a photo of the screen.
   - Whether Enforcer is installed. If it is, what its control panel
     lists under Caught.

## Report a security problem

A page that can crash Aura, write outside its own memory, read a file,
get past a certificate error, or read another site's data is a security
problem. Open an issue that says you have one, without the details, and
I will arrange a private way to send them.

## Fix a bug

Using an AI assistant is welcome here: this project was written with
one. This is how I would like a fix to arrive, with or without one:

1. Open an issue first, as above. If you use an AI assistant, have it
   write down what it found: where the fault is, how it knows, and
   anything else it thinks matters.
2. Send a pull request that fixes that issue, and name the issue in it.
3. Keep the change to that one problem. No reformatting, renaming or
   other improvements in the same pull request; open another issue for
   those.
4. Add a test that fails before the fix and passes after it. Every area
   has a folder in `tests`.
5. Test it, and say in the pull request what you ran and what you saw:
   - `./build.sh test`, which builds for your machine and runs the
     tests.
   - The tests under AddressSanitizer and UBSan, for any change to code
     that reads data from the network or a file (`docs/building.md`,
     "Other build trees").
   - `./build.sh ppc`, which builds the Mac OS 9 application. It must
     build without warnings.
   - The application itself, in QEMU or on a real Mac. Say which, and
     which Mac OS version.
6. Start the commit subject with `fix:` so the change is listed in the
   next release (`docs/conventions.md`, "Commit messages").

If you or your assistant could not test something, say so. An honest
"not run on Mac OS 9" is better than a claim nobody checked.

`README.md` says how to build Aura. `CLAUDE.md` is the starting point
for development: it states the rules every change follows and names the
document under `docs/` for each kind of work (the conventions, how each
part works, how to set up and drive the test machine, what is not done
yet, and what was learned on the way). Point your assistant at it
before it changes anything.

A few rules that reviews will hold to:

- The engine is portable C99 and makes no operating system calls. It
  must build for a modern machine and for PowerPC Mac OS 9.
- Everything that comes from a web page, a server or a file on disk is
  hostile. Check every length, bound every loop, and do not recurse
  over the depth of a document.
- Aura uses two third-party libraries, QuickJS-ng and Mbed TLS. Ask
  before adding another.

## Add or change a feature

Open an issue before you write the code. Say:

- What you want to add or change.
- Why: the problem it solves, and for whom.
- How it should behave.
- What it costs: memory, speed on a G4, and the size of the
  application.

`docs/v1-scope.md` lists what the first version aims for and what it
leaves out. I am open to anything that makes more of the web work on
Mac OS 9, makes Aura faster or smaller, or makes it safer. A feature
outside those is less likely to go in, so ask first.

## What stays out of the repository

- Apple's files: no system software, no disk images, no headers or
  fonts from Apple.
- Code or data under the GPL or another licence that does not fit MIT.
  That is why Aura downloads blocklists and does not ship them.
- Fonts without a licence that allows redistribution. Each bundled font
  has its licence in its folder.

## License

Aura is under the MIT License (`LICENSE`). By contributing, you agree
that your contribution is under the same license.
