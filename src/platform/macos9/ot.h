/* Open Transport declarations for the Mac OS 9 networking code.
 *
 * The Multiversal Interfaces have no Open Transport headers, and Apple's
 * are not redistributable, so this file declares the slice Aura uses.
 * The types carry Aura's own names (AuOt...), so they cannot collide with
 * Apple's if those headers ever appear; the functions keep the names the
 * system libraries export (OpenTransportLib, OTClientLib, OTUtilityLib,
 * OTInetClientLib), which the Retro68 import libraries
 * libOpenTransportLib.a and libOpenTptInternetLib.a resolve.
 *
 * Apple's static glue (OpenTransportAppPPC.o) implemented InitOpenTransport,
 * OTAsyncOpenEndpoint and the other calls that create an Open Transport
 * object as wrappers around exported "Priv" routines that take one more
 * argument: the address of the application's client record. Aura has no
 * glue library; net_ot.c keeps the record (AuOtClient) and calls the Priv
 * routines itself. The record layout and the flags value are as described
 * in Apple's Technical Note TN1173 and observed in the system: 40 bytes,
 * zeroed, with words at +8, +12 and +16 pointing at three words the
 * application owns and the byte at +32 set to 1; flags 3 registers an
 * application client, which Open Transport closes for it at ExitToShell.
 *
 * Event codes, errors and structure layouts are from Inside Macintosh:
 * Networking With Open Transport. */
#ifndef AURA_OT_H
#define AURA_OT_H

#include <Multiverse.h>

typedef void *AuOtRef; /* ProviderRef, EndpointRef, InetSvcRef */
typedef struct AuOtConfig AuOtConfig; /* OTConfiguration */
typedef UInt32 AuOtEvent;
typedef SInt32 AuOtResult;
typedef void (*AuOtNotifyProc)(void *ctx, AuOtEvent code, AuOtResult result, void *cookie);

typedef struct AuOtClient {
    UInt32 os_a[2];
    UInt32 *word8;   /* +8 */
    UInt32 *word12;  /* +12 */
    UInt32 *word16;  /* +16 */
    UInt32 os_b[3];
    UInt8 flag;      /* +32, 1 */
    UInt8 pad[7];
} AuOtClient;

typedef struct AuOtNetbuf {
    UInt32 maxlen;
    UInt32 len;
    UInt8 *buf;
} AuOtNetbuf;

typedef struct AuOtCall {
    AuOtNetbuf addr;
    AuOtNetbuf opt;
    AuOtNetbuf udata;
    SInt32 sequence;
} AuOtCall;

typedef struct AuOtInetAddress {
    UInt16 fAddressType; /* AF_INET, 2 */
    UInt16 fPort;
    UInt32 fHost;
    UInt8 fUnused[8];
} AuOtInetAddress;

typedef struct AuOtInetHostInfo {
    char name[256];
    UInt32 addrs[10];
} AuOtInetHostInfo;

enum {
    kAuOtInitApplication = 3,
    kAuOtAfInet = 2,
    /* Gestalt 'otan' bits */
    kAuOtPresent = 0x00000001,
    kAuOtTcpPresent = 0x00000010
};

/* Events (OTEventCode). */
enum {
    kAuOtEvConnect = 0x0002,
    kAuOtEvData = 0x0004,
    kAuOtEvDisconnect = 0x0010,
    kAuOtEvOrdRel = 0x0080,
    kAuOtEvGoData = 0x0100
};
#define kAuOtEvBindComplete ((AuOtEvent)0x20000001UL)
#define kAuOtEvOpenComplete ((AuOtEvent)0x20000007UL)
#define kAuOtEvDnrStringToAddrComplete ((AuOtEvent)0x10000001UL)

/* Results. */
enum {
    kAuOtOutStateErr = -3155,
    kAuOtLookErr = -3158,
    kAuOtFlowErr = -3161,
    kAuOtNoDataErr = -3162
};

#define kAuOtDefaultInternetServices ((AuOtConfig *)-3L)

extern pascal OSStatus InitOpenTransportPriv(UInt32 flags, AuOtClient *client);
extern pascal void CloseOpenTransportPriv(AuOtClient *client);
/* The configuration passed to an open call belongs to Open Transport from
 * then on, whether the open succeeds or not. */
extern pascal AuOtConfig *OTCreateConfiguration(const char *path);
extern pascal OSStatus OTAsyncOpenEndpointPriv(AuOtConfig *cfg, UInt32 oflag, void *info, AuOtNotifyProc proc,
                                               void *ctx, AuOtClient *client);
extern pascal OSStatus OTAsyncOpenInternetServicesPriv(AuOtConfig *cfg, UInt32 oflag, AuOtNotifyProc proc, void *ctx,
                                                       AuOtClient *client);
extern pascal OSStatus OTCloseProviderPriv(AuOtRef ref, AuOtClient *client);
extern pascal OSStatus OTSetNonBlocking(AuOtRef ref);
extern pascal OSStatus OTBind(AuOtRef ep, void *req, void *ret);
extern pascal OSStatus OTConnect(AuOtRef ep, AuOtCall *snd, AuOtCall *rcv);
extern pascal OSStatus OTRcvConnect(AuOtRef ep, AuOtCall *call);
extern pascal OSStatus OTRcvDisconnect(AuOtRef ep, void *discon);
extern pascal OSStatus OTRcvOrderlyDisconnect(AuOtRef ep);
extern pascal AuOtResult OTLook(AuOtRef ep);
extern pascal AuOtResult OTSnd(AuOtRef ep, void *buf, UInt32 n, UInt32 flags);
extern pascal AuOtResult OTRcv(AuOtRef ep, void *buf, UInt32 n, UInt32 *flags);
extern pascal OSStatus OTInetStringToAddress(AuOtRef svc, char *name, AuOtInetHostInfo *out);

/* InterfaceLib routines the Multiversal headers leave out. */
extern pascal OSErr HoldMemory(void *address, unsigned long count);
extern pascal OSErr UnholdMemory(void *address, unsigned long count);

#endif
