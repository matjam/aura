/* Entropy from the system's generator (see posix.h): getentropy where the
 * C library declares it (macOS 10.12, glibc 2.25, musl 1.1.20, the BSDs),
 * else /dev/urandom. getentropy is also tried at run time first, and
 * /dev/urandom used when it fails (an old kernel without getrandom). */
#include "posix.h"

#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/random.h>
#define AU_HAVE_GETENTROPY 1
#elif defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)
#define AU_HAVE_GETENTROPY 1
#elif defined(__linux__) && defined(__GLIBC__)
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25)
#define AU_HAVE_GETENTROPY 1
#endif
#elif defined(__linux__)
/* musl declares getentropy in unistd.h from 1.1.20 on. */
#define AU_HAVE_GETENTROPY 1
#endif

static bool from_urandom(uint8_t *p, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            close(fd);
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return true;
}

bool au_posix_entropy(void *buf, size_t len)
{
    uint8_t *p = buf;
#ifdef AU_HAVE_GETENTROPY
    /* getentropy gives at most 256 bytes a call. */
    size_t done = 0;
    while (done < len) {
        size_t n = len - done > 256 ? 256 : len - done;
        if (getentropy(p + done, n) != 0)
            break;
        done += n;
    }
    if (done == len)
        return true;
    return from_urandom(p + done, len - done);
#else
    return from_urandom(p, len);
#endif
}
