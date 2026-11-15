/* Aura Installer resources: the partition, the alerts and the Finder
 * bundle. The icon family (ID 128, and the custom icon at -16455) is
 * Aura's, from ../aura_icon.r; the 'vers' resource is generated from
 * AU_VERSION (sea_vers.r.in). */
#include "Processes.r"
#include "Dialogs.r"
#include "Finder.r"

/* The inflate decoder (45 KB), two buffers (48 KB) and the Toolbox. */
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
	1024 * 1024,
	768 * 1024
};

/* 128: an error. ^0 what happened, ^1 the detail. */
resource 'ALRT' (128) {
	{0, 0, 150, 400}, 128,
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
		{118, 318, 138, 386}, Button { enabled, "OK" };
		{12, 70, 110, 386}, StaticText { disabled, "^0\0x0D\0x0D^1" }
	}
};

/* 129: the chosen name is taken. ^0 the name, ^1 the free name. */
resource 'ALRT' (129) {
	{0, 0, 130, 400}, 129,
	{
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1;
		OK, visible, sound1
	},
	alertPositionMainScreen
};

resource 'DITL' (129) {
	{
		{98, 318, 118, 386}, Button { enabled, "OK" };
		{98, 238, 118, 306}, Button { enabled, "Cancel" };
		{12, 70, 90, 386}, StaticText { disabled,
			"\0xD2^0\0xD3 is already there. Nothing is replaced: "
			"put the folder there as \0xD2^1\0xD3?" }
	}
};

/* The Finder bundle for creator 'AuSX': the application's icon. */
resource 'BNDL' (128) {
	'AuSX', 0,
	{
		'ICN#', { 0, 128 };
		'FREF', { 0, 128 }
	}
};

resource 'FREF' (128) {
	'APPL', 0, ""
};

data 'AuSX' (0, "Owner resource") {
	$"0E 41 75 72 61 20 49 6E 73 74 61 6C 6C 65 72"   /* "Aura Installer" */
};
