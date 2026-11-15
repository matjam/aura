/* Internet Config declarations that the Multiversal Interfaces leave out,
 * from Apple's InternetConfig.h (Internet Config 2.0 and later). Retro68
 * ships a stub library for InternetConfigLib, but Mac OS 9.2.2 has no
 * shared library of that name, only the Internet Config component, so
 * internetconfig.c calls the component through the Component Manager.
 *
 * Internet Config's records use 68K alignment (2-byte), so they are
 * declared inside #pragma pack(2): with PowerPC alignment the OSType
 * after three shorts in ICMapEntry would move. */
#ifndef AURA_MAC_INTERNETCONFIG_H
#define AURA_MAC_INTERNETCONFIG_H

#include <Multiverse.h>

typedef UInt32 ICAttr;

#define kAuICAttrNoChange ((ICAttr)0xFFFFFFFF)
enum { kAuICPrefNotFoundErr = -666 };

#pragma pack(push, 2)

typedef struct ICAppSpec {
    OSType fCreator;
    Str63 name;
} ICAppSpec;

typedef struct ICFileSpec {
    Str31 volName;
    SInt32 volCreationDate;
    FSSpec fss;
    /* an AliasRecord follows (variable length) */
} ICFileSpec;

typedef struct ICMapEntry {
    SInt16 totalLength;
    UInt16 fixedLength;
    SInt16 version;
    OSType fileType;
    OSType fileCreator;
    OSType postCreator;
    SInt32 flags;
    Str255 extension;
    Str255 creatorAppName;
    Str255 postAppName;
    Str255 MIMEType;
    Str255 entryName;
} ICMapEntry;

#pragma pack(pop)

/* The Component Manager, which the Multiversal Interfaces also lack
 * (Apple's Components.h); InterfaceLib exports it. */
typedef struct ComponentRecord *Component;
typedef struct ComponentInstanceRecord *ComponentInstance;
typedef struct ComponentDescription {
    OSType componentType;
    OSType componentSubType;
    OSType componentManufacturer;
    UInt32 componentFlags;
    UInt32 componentFlagsMask;
} ComponentDescription;
pascal Component FindNextComponent(Component aComponent, ComponentDescription *looking);
pascal ComponentInstance OpenDefaultComponent(OSType componentType, OSType componentSubType);
pascal OSErr CloseComponent(ComponentInstance aComponentInstance);
extern UniversalProcPtr CallComponentUPP;

#endif
