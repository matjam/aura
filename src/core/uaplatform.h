/* The platform Aura says it runs on: the part in parentheses of the user
 * agent, navigator.platform and navigator.oscpu. It must be the platform
 * the build runs on: a Mac OS 9 platform part sent from macOS or Linux is
 * wrong, and sites treat it as suspect (Slashdot's Cloudflare rule
 * challenges "PPC Mac OS 9" on every request, 2026-11-17).
 *
 * CMake sets AU_UA_KIND for the platform being built (AURA_PLATFORM, and
 * for the host build the system it runs on); a number, because CMake
 * splits definitions at semicolons. The values follow what the mainstream
 * browsers send: macOS on any processor is "Intel Mac OS X 10_15_7", the
 * version Chrome and Safari froze; Linux names its processor. */
#ifndef AURA_CORE_UAPLATFORM_H
#define AURA_CORE_UAPLATFORM_H

#define AU_UA_KIND_MACOS9 0
#define AU_UA_KIND_MACOS 1
#define AU_UA_KIND_LINUX_X86_64 2
#define AU_UA_KIND_LINUX_AARCH64 3

#ifndef AU_UA_KIND
#define AU_UA_KIND AU_UA_KIND_MACOS9
#endif

#if AU_UA_KIND == AU_UA_KIND_MACOS
#define AU_UA_PLATFORM "Macintosh; Intel Mac OS X 10_15_7"
#define AU_NAV_PLATFORM "MacIntel"
#define AU_NAV_OSCPU "Intel Mac OS X 10.15"
#elif AU_UA_KIND == AU_UA_KIND_LINUX_X86_64
#define AU_UA_PLATFORM "X11; Linux x86_64"
#define AU_NAV_PLATFORM "Linux x86_64"
#define AU_NAV_OSCPU "Linux x86_64"
#elif AU_UA_KIND == AU_UA_KIND_LINUX_AARCH64
#define AU_UA_PLATFORM "X11; Linux aarch64"
#define AU_NAV_PLATFORM "Linux aarch64"
#define AU_NAV_OSCPU "Linux aarch64"
#else
#define AU_UA_PLATFORM "Macintosh; PPC Mac OS 9"
#define AU_NAV_PLATFORM "MacPPC"
#define AU_NAV_OSCPU "PPC Mac OS 9"
#endif

#endif
