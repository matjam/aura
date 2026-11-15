/* Contextual menus: the Contextual Menu Manager when the system has it,
 * else a pop-up menu at the click (PopUpMenuSelect).
 *
 * The Multiversal Interfaces do not declare the Contextual Menu Manager,
 * so its functions are found at run time in the ContextualMenu shared
 * library (part of Mac OS 8 and later) with the Code Fragment Manager.
 * Without the library Aura still launches, and Control-click shows the
 * pop-up menu. The prototypes follow Apple's Menus.h. */
#include "mac.h"

#include <string.h>

typedef pascal OSStatus (*InitContextualMenusProc)(void);
typedef pascal Boolean (*IsShowContextualMenuClickProc)(const EventRecord *inEvent);
typedef pascal OSStatus (*ContextualMenuSelectProc)(MenuHandle inMenu, Point inGlobalLocation, Boolean inReserved,
                                                     UInt32 inHelpType, ConstStr255Param inHelpItemString,
                                                     const AEDesc *inSelection, UInt32 *outUserSelectionType,
                                                     SInt16 *outMenuID, UInt16 *outMenuItem);

enum { kAuCMHelpItemNoHelp = 0, kAuCMMenuItemSelected = 1 };
enum { kContextMenuID = 180 }; /* clear of the folder menus (200 to 255) */

static IsShowContextualMenuClickProc cm_is_click;
static ContextualMenuSelectProc cm_select;

void mac_context_init(void)
{
    ConnectionID conn;
    Ptr main_addr, sym;
    Str255 err;
    SymClass cls;
    if (GetSharedLibrary((ConstStr63Param) "\pContextualMenu", kPowerPCArch, kReferenceCFrag, &conn, &main_addr,
                         err) != noErr)
        return;
    InitContextualMenusProc init = NULL;
    if (FindSymbol(conn, (ConstStr255Param) "\pInitContextualMenus", &sym, &cls) == noErr)
        init = (InitContextualMenusProc)sym;
    if (FindSymbol(conn, (ConstStr255Param) "\pIsShowContextualMenuClick", &sym, &cls) == noErr)
        cm_is_click = (IsShowContextualMenuClickProc)sym;
    if (FindSymbol(conn, (ConstStr255Param) "\pContextualMenuSelect", &sym, &cls) == noErr)
        cm_select = (ContextualMenuSelectProc)sym;
    if (!init || !cm_is_click || !cm_select || init() != noErr)
        cm_is_click = NULL, cm_select = NULL;
}

bool mac_context_click(const EventRecord *ev)
{
    if (cm_is_click)
        return cm_is_click(ev) != 0;
    return (ev->modifiers & ControlKey) != 0;
}

int mac_context_select(const AuContextMenu *m, Point global)
{
    if (m->n == 0)
        return -1;
    MenuHandle menu = NewMenu(kContextMenuID, (ConstStringPtr) "\p");
    if (!menu)
        return -1;
    for (int i = 0; i < m->n; i++) {
        if (m->items[i].cmd == AU_CMD_SEPARATOR) {
            AppendMenu(menu, (ConstStringPtr) "\p(-");
            continue;
        }
        mac_append_item(menu, m->items[i].label);
        if (!m->items[i].enabled)
            DisableItem(menu, (short)(i + 1));
    }
    InsertMenu(menu, -1);
    short item = 0;
    if (cm_select) {
        UInt32 type = 0;
        SInt16 id = 0;
        UInt16 it = 0;
        OSStatus err = cm_select(menu, global, false, kAuCMHelpItemNoHelp, NULL, NULL, &type, &id, &it);
        if (err == noErr && type == kAuCMMenuItemSelected && id == kContextMenuID)
            item = (short)it;
    } else {
        long r = PopUpMenuSelect(menu, global.v, global.h, 0);
        if ((short)(r >> 16) == kContextMenuID)
            item = (short)(r & 0xFFFF);
    }
    DeleteMenu(kContextMenuID);
    DisposeMenu(menu);
    return item >= 1 && item <= m->n ? item - 1 : -1;
}
