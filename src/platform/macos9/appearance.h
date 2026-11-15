/* Appearance Manager declarations that the Multiversal Interfaces leave
 * out, from Apple's Appearance.h, Controls.h and ControlDefinitions.h
 * (Appearance 1.0.1 and later; Mac OS 9.2.2 has 1.1.4). The functions are
 * exported by AppearanceLib, which CMakeLists.txt links. */
#ifndef AURA_MAC_APPEARANCE_H
#define AURA_MAC_APPEARANCE_H

#include <Multiverse.h>

typedef SInt16 ControlPartCodeA;
typedef unsigned short AuEventModifiers; /* EventModifiers */
typedef SInt16 ControlFocusPartA;
typedef SInt16 ThemeBrushA;

/* Control definition procIDs. */
enum {
    kAuControlUserPaneProc = 256,
    kAuControlTabSmallProc = 129,
    kAuControlStaticTextProc = 288,
    kAuControlEditTextProc = 272,
    kAuControlGroupBoxTextTitleProc = 160,
    kAuControlPushButtonProc = 368,
    kAuControlCheckBoxAutoToggleProc = 371,
    kAuControlRadioButtonProc = 370,
    kAuControlPopupButtonProc = 400
};

/* User pane feature flags (the control's initial value). */
enum { kAuControlSupportsEmbedding = 1 << 1 };

/* Parts and focus. */
enum {
    kAuControlEditTextPart = 5,
    kAuControlFocusNoPart = 0,
    kAuControlFocusNextPart = -1
};

/* Control data tags. */
enum {
    kAuControlEditTextTextTag = 'text',
    kAuControlEditTextTEHandleTag = 'than',
    kAuControlStaticTextTextTag = 'text',
    kAuControlPushButtonDefaultTag = 'dflt'
};

enum { kAuThemeBrushDialogBackgroundActive = 1 };

/* ControlFontStyleRec */
typedef struct AuControlFontStyleRec {
    SInt16 flags, font, size, style, mode, just;
    RGBColor foreColor, backColor;
} AuControlFontStyleRec;
enum { kAuControlUseFontMask = 0x0001, kAuControlUseSizeMask = 0x0004 };
enum { kAuControlFontSmallSystemFont = -2 };

pascal OSErr RegisterAppearanceClient(void);
pascal OSErr UnregisterAppearanceClient(void);
pascal OSErr CreateRootControl(WindowPtr inWindow, ControlHandle *outControl);
pascal OSErr EmbedControl(ControlHandle inControl, ControlHandle inContainer);
pascal OSErr SetControlData(ControlHandle inControl, ControlPartCodeA inPart, ResType inTagName, Size inSize,
                            Ptr inData);
pascal OSErr GetControlData(ControlHandle inControl, ControlPartCodeA inPart, ResType inTagName,
                            Size inBufferSize, Ptr inBuffer, Size *outActualSize);
pascal OSErr GetControlDataSize(ControlHandle inControl, ControlPartCodeA inPart, ResType inTagName,
                                Size *outMaxSize);
pascal OSErr SetKeyboardFocus(WindowPtr inWindow, ControlHandle inControl, ControlFocusPartA inPart);
pascal OSErr GetKeyboardFocus(WindowPtr inWindow, ControlHandle *outControl);
pascal OSErr AdvanceKeyboardFocus(WindowPtr inWindow);
pascal ControlPartCodeA HandleControlKey(ControlHandle inControl, SInt16 inKeyCode, SInt16 inCharCode,
                                         AuEventModifiers inModifiers);
pascal void IdleControls(WindowPtr inWindow);
pascal ControlPartCodeA HandleControlClick(ControlHandle inControl, Point inWhere, AuEventModifiers inModifiers,
                                           ControlActionUPP inAction);
pascal ControlHandle FindControlUnderMouse(Point inWhere, WindowPtr inWindow, SInt16 *outPart);
pascal OSErr SetControlVisibility(ControlHandle inControl, Boolean inIsVisible, Boolean inDoDraw);
pascal OSErr ActivateControl(ControlHandle inControl);
pascal OSErr DeactivateControl(ControlHandle inControl);
pascal OSErr SetThemeWindowBackground(WindowPtr inWindow, ThemeBrushA inBrush, Boolean inUpdate);
pascal OSErr SetControlFontStyle(ControlHandle inControl, const AuControlFontStyleRec *inStyle);

#endif
