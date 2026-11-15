/* Aura's resources: memory partition, menus, alerts, dialogs, the
   preferences tab names, version, Finder bundle. */
#include "Processes.r"
#include "Menus.r"
#include "Dialogs.r"
#include "Finder.r"

/* Memory: the application heap holds the engine's working memory for now
   (au_alloc is newlib's malloc, which allocates in this heap). Pages will
   later allocate their memory in temporary memory (TempNewHandle) and the
   GWorlds already prefer it, so the partition does not have to grow with
   the page. 32 MB preferred, 12 MB minimum. */
resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	isHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	32 * 1024 * 1024,
	12 * 1024 * 1024
};

resource 'MBAR' (128) {
	{ 128, 129, 130, 131, 132, 133, 134 }
};

resource 'MENU' (128, "Apple") {
	128, textMenuProc, 0x7FFFFFFD, enabled, apple,
	{
		"About Aura...", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (129, "File") {
	129, textMenuProc, 0x7FFFFF6F, enabled, "File",
	{
		"New Window", noIcon, "N", noMark, plain;
		"New Tab", noIcon, "T", noMark, plain;
		"Open Location...", noIcon, "L", noMark, plain;
		"Open File...", noIcon, "O", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Close Tab", noIcon, "W", noMark, plain;
		"Close Window", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Quit", noIcon, "Q", noMark, plain
	}
};

resource 'MENU' (130, "Edit") {
	130, textMenuProc, 0x7FFFFF7D, enabled, "Edit",
	{
		"Undo", noIcon, "Z", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Cut", noIcon, "X", noMark, plain;
		"Copy", noIcon, "C", noMark, plain;
		"Paste", noIcon, "V", noMark, plain;
		"Clear", noIcon, noKey, noMark, plain;
		"Select All", noIcon, "A", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Preferences...", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (131, "View") {
	131, textMenuProc, 0x7FFFFFDB, enabled, "View",
	{
		"Reload", noIcon, "R", noMark, plain;
		"Stop", noIcon, ".", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Tabs on Top", noIcon, noKey, noMark, plain;
		"Tabs on Left", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Debug", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (135, "Debug") {
	135, textMenuProc, 0x7FFFFFFF, enabled, "Debug",
	{
		"Test Scene", noIcon, noKey, noMark, plain;
		"Font List", noIcon, noKey, noMark, plain;
		"Benchmark", noIcon, noKey, noMark, plain;
		"Start Simulated Downloads", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (132, "Go") {
	132, textMenuProc, 0x7FFFFFF7, enabled, "Go",
	{
		"Back", noIcon, "[", noMark, plain;
		"Forward", noIcon, "]", noMark, plain;
		"Home", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Show History", noIcon, "Y", noMark, plain
	}
};

resource 'MENU' (133, "Bookmarks") {
	133, textMenuProc, 0x7FFFFFFB, enabled, "Bookmarks",
	{
		"Add Bookmark", noIcon, "D", noMark, plain;
		"Show Bookmarks", noIcon, "B", noMark, plain;
		"-", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (134, "Window") {
	134, textMenuProc, 0x7FFFFFFF, enabled, "Window",
	{
		"Downloads", noIcon, "J", noMark, plain
	}
};

/* The Preferences window's tabs (Appearance 'tab#': version, then per
   tab an icon suite ID, the name and two reserved fields). */
type 'tab#' {
	integer = 0;
	integer = $$CountOf(TabInfo);
	array TabInfo {
		integer;
		pstring;
		fill long;
		fill word;
	};
};

resource 'tab#' (128) {
	{
		0, "General";
		0, "Fonts";
		0, "Content";
		0, "Privacy & Security";
		0, "Network";
		0, "Downloads"
	}
};

/* 128: About. */
resource 'ALRT' (128) {
	{0, 0, 120, 360}, 128,
	{
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1
	},
	alertPositionMainScreen
};

resource 'DITL' (128) {
	{
		{88, 278, 108, 346}, Button { enabled, "OK" };
		{12, 70, 80, 346}, StaticText { disabled,
			"Aura 0.2\0x0DA web browser for Mac OS 9.\0x0D"
			"Browser chrome test build: the page engine is not connected yet." }
	}
};

/* 129: benchmark results, ^0 to ^3. */
resource 'ALRT' (129) {
	{0, 0, 170, 400}, 129,
	{
		OK, visible, silent;
		OK, visible, silent;
		OK, visible, silent;
		OK, visible, silent
	},
	alertPositionMainScreen
};

resource 'DITL' (129) {
	{
		{138, 318, 158, 386}, Button { enabled, "OK" };
		{12, 70, 130, 386}, StaticText { disabled, "^0\0x0D^1\0x0D^2\0x0D^3" }
	}
};

/* 130: out of memory. */
resource 'ALRT' (130) {
	{0, 0, 110, 360}, 130,
	{
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1
	},
	alertPositionMainScreen
};

resource 'DITL' (130) {
	{
		{78, 278, 98, 346}, Button { enabled, "Quit" };
		{12, 70, 70, 346}, StaticText { disabled,
			"Aura ran out of memory (^0 bytes requested) and has to quit. "
			"Give it a larger partition in the Finder's Get Info window." }
	}
};

/* 200: asks for a name (new bookmark folder, rename); ^0 is the prompt. */
resource 'DLOG' (200) {
	{0, 0, 110, 340}, movableDBoxProc, invisible, noGoAway, 0, 200, "Name",
	alertPositionMainScreen
};

resource 'DITL' (200) {
	{
		{78, 254, 98, 326}, Button { enabled, "OK" };
		{78, 168, 98, 240}, Button { enabled, "Cancel" };
		{40, 16, 56, 324}, EditText { enabled, "" };
		{12, 16, 30, 324}, StaticText { disabled, "^0" }
	}
};

resource 'vers' (1) {
	0x00, 0x02, development, 0x00, verUS,
	"0.2",
	"0.2, Aura browser chrome"
};

resource 'BNDL' (128) {
	'AURA', 0,
	{
		'FREF', { 0, 128 }
	}
};

resource 'FREF' (128) {
	'APPL', 0, ""
};

data 'AURA' (0) {
	$"04 41 75 72 61"
};
