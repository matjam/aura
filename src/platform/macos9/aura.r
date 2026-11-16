/* Aura's resources: memory partition, alerts, dialogs, the preferences
   tab names, version, Finder bundle. */
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
	64 * 1024 * 1024,
	12 * 1024 * 1024
};

/* The menus are built at start-up from the command table
   (src/browser/commands.c); main.c's build_menu_bar makes them. */

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
		0, "Downloads";
		0, "Blocking"
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

/* 132: a file error; ^0 says what failed. */
resource 'ALRT' (132) {
	{0, 0, 100, 360}, 132,
	{
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1
	},
	alertPositionMainScreen
};

resource 'DITL' (132) {
	{
		{68, 278, 88, 346}, Button { enabled, "OK" };
		{12, 70, 60, 346}, StaticText { disabled, "^0" }
	}
};

/* 133: offers to restore the last session after an unclean exit. */
resource 'ALRT' (133) {
	{0, 0, 120, 380}, 133,
	{
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1
	},
	alertPositionMainScreen
};

resource 'DITL' (133) {
	{
		{88, 278, 108, 366}, Button { enabled, "Restore" };
		{88, 156, 108, 266}, Button { enabled, "Don't Restore" };
		{12, 70, 80, 366}, StaticText { disabled,
			"Aura did not quit normally last time. Restore the windows and tabs "
			"that were open?" }
	}
};

/* 134: "warn before downloading applications"; ^0 is the file name. */
resource 'ALRT' (134) {
	{0, 0, 130, 400}, 134,
	{
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1
	},
	alertPositionMainScreen
};

resource 'DITL' (134) {
	{
		{98, 296, 118, 386}, Button { enabled, "Download" };
		{98, 196, 118, 284}, Button { enabled, "Cancel" };
		{12, 70, 90, 386}, StaticText { disabled,
			"\"^0\" looks like an application or installer. Download it anyway? "
			"Aura saves it in the download folder and never opens it." }
	}
};

/* 136: a yes or no question from a page (resending a form); ^0 is it. */
resource 'ALRT' (136) {
	{0, 0, 130, 400}, 136,
	{
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1
	},
	alertPositionMainScreen
};

resource 'DITL' (136) {
	{
		{98, 296, 118, 386}, Button { enabled, "Continue" };
		{98, 196, 118, 284}, Button { enabled, "Cancel" };
		{12, 70, 90, 386}, StaticText { disabled, "^0" }
	}
};

/* 135: a cookie's or site's details in the Cookies window, ^0 to ^3. */
resource 'ALRT' (135) {
	{0, 0, 250, 460}, 135,
	{
		OK, visible, silent;
		OK, visible, silent;
		OK, visible, silent;
		OK, visible, silent
	},
	alertPositionMainScreen
};

resource 'DITL' (135) {
	{
		{218, 378, 238, 446}, Button { enabled, "OK" };
		{12, 70, 210, 446}, StaticText { disabled, "^0^1^2^3" }
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

/* 202: About Aura. Every text is drawn by aboutbox.c: the heading (name,
 * version, tagline), the home page link, and the facts and the quip. The
 * icon and the link are enabled user items, so a click on them returns. */
resource 'DLOG' (202) {
	{0, 0, 300, 460}, movableDBoxProc, invisible, noGoAway, 0, 202, "About Aura",
	alertPositionMainScreen
};

/* An empty colour table: its presence makes the Dialog Manager give the
 * About box a colour port, so the icon draws in colour. */
data 'dctb' (202) {
	$"00000000 0000 FFFF"
};

resource 'DITL' (202) {
	{
		{264, 374, 284, 444}, Button { enabled, "OK" };
		{264, 206, 284, 362}, Button { enabled, "Credits & Licences..." };
		{18, 18, 82, 82}, UserItem { enabled };
		{14, 100, 104, 444}, UserItem { disabled };
		{106, 100, 122, 300}, UserItem { enabled };
		{136, 18, 252, 444}, UserItem { disabled }
	}
};

/* 201: Find (Edit > Find...). */
resource 'DLOG' (201) {
	{0, 0, 120, 340}, movableDBoxProc, invisible, noGoAway, 0, 201, "Find",
	alertPositionMainScreen
};

resource 'DITL' (201) {
	{
		{88, 254, 108, 326}, Button { enabled, "Find" };
		{88, 168, 108, 240}, Button { enabled, "Cancel" };
		{36, 16, 52, 324}, EditText { enabled, "" };
		{12, 16, 30, 324}, StaticText { disabled, "Find text in this page:" };
		{60, 16, 78, 324}, CheckBox { enabled, "Match case" }
	}
};

/* The 'vers' resource is generated from AU_VERSION (aura_vers.r.in). */

/* The bundle: the application, and the document types it opens, so the
   Finder lets text and HTML files be dropped on its icon. Local icon 1
   (the document types) has no icon of its own: the Finder draws those
   files with their own creator's icon. */
resource 'BNDL' (128) {
	'AURA', 0,
	{
		'ICN#', { 0, 128 },
		'FREF', { 0, 128; 1, 129; 2, 130 }
	}
};

resource 'FREF' (128) {
	'APPL', 0, ""
};

resource 'FREF' (129) {
	'TEXT', 1, ""
};

resource 'FREF' (130) {
	'****', 1, ""
};

data 'AURA' (0) {
	$"04 41 75 72 61"
};
