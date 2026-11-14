/* Aura's resources: memory partition, menus, alerts, version, Finder
   bundle. */
#include "Processes.r"
#include "Menus.r"
#include "Dialogs.r"
#include "Finder.r"

/* Memory: the application heap holds the engine's working memory for now
   (au_alloc is newlib's malloc, which allocates in this heap). Pages will
   later allocate their memory in temporary memory (TempNewHandle) and the
   GWorld already prefers it, so the partition does not have to grow with
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
	{ 128, 129, 130 }
};

resource 'MENU' (128, "Apple") {
	128, textMenuProc, 0x7FFFFFFD, enabled, apple,
	{
		"About Aura...", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (129, "File") {
	129, textMenuProc, 0x7FFFFFFF, enabled, "File",
	{
		"Test Scene", noIcon, "T", noMark, plain;
		"Font List", noIcon, "F", noMark, plain;
		"Benchmark", noIcon, "B", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Quit", noIcon, "Q", noMark, plain
	}
};

resource 'MENU' (130, "Edit") {
	130, textMenuProc, 0x7FFFFFC0, enabled, "Edit",
	{
		"Undo", noIcon, "Z", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Cut", noIcon, "X", noMark, plain;
		"Copy", noIcon, "C", noMark, plain;
		"Paste", noIcon, "V", noMark, plain;
		"Clear", noIcon, noKey, noMark, plain
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
			"Aura\nA web browser engine for Mac OS 9.\n"
			"Paint, font and window test build." }
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
		{12, 70, 130, 386}, StaticText { disabled, "^0\n^1\n^2\n^3" }
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

resource 'vers' (1) {
	0x00, 0x01, development, 0x00, verUS,
	"0.1",
	"0.1, Aura paint and font test"
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
