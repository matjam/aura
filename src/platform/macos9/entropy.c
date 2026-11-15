/* The Mac OS 9 entropy pool: sources, credit, seed file. See entropy.h.
 *
 * Pool. A 32-byte state S. Mixing input D sets S = SHA-256("mix" || S ||
 * n || D), with n a counter. A read produces blocks SHA-256("out" || S ||
 * n), then moves the state on with S = SHA-256("next" || S), so earlier
 * output cannot be recomputed from a later state.
 *
 * Credit, in 1/64 bit, capped at 256 bits:
 *   seed file written by a run that had reached the threshold  64 bits
 *   start-up jitter run: 1/16 bit per sample whose third-order
 *     timing difference is not zero, at most                    32 bits
 *   loop jitter (au_mac_entropy_idle): 1/8 bit per sample whose
 *     third-order difference is not zero, at most one sample per
 *     tick (1/60 s), so at most 7.5 bits a second
 *   mouse moved since the last pass                              1 bit
 *   key and mouse button events (with their microsecond time)    2 bits
 *   clocks, free memory, null events, network timings            0
 * The jitter estimate follows the usual practice of jitter collectors:
 * a sample whose third difference is zero is taken as predictable. On
 * QEMU the clock comes from the host and jitters freely; on a real Mac
 * the main loop's timing varies with interrupts, disk and network, and
 * the credit per sample stays far below what one sample can hold.
 *
 * Without a seed file and without user input, the pool reaches 128 bits
 * after the start-up run plus 768 credited loop samples: at least 13 s,
 * about 15 s with the loop at 50 passes a second. A valid seed file
 * takes 64 bits off that. */
#include "entropy.h"

#include "bearssl.h"
#include "core/core.h"

#include <string.h>

/* Microseconds is in InterfaceLib; the Multiversal headers leave it out. */
typedef struct {
    uint32_t hi, lo;
} AuMacWide;
extern pascal void Microseconds(AuMacWide *us);
extern pascal uint32_t TickCount(void);

enum {
    kBit = 64,
    kMaxCredit = 256 * kBit,
    kSeedCredit = 64 * kBit,
    kStartupMax = 32 * kBit,
    kStartupSample = kBit / 16,
    kJitterSample = kBit / 8,
    kMouseCredit = kBit,
    kInputCredit = 2 * kBit,
    kStartupSamples = 4096,
    kSeedLen = 32
};

static const char kSeedMagic[8] = {'A', 'U', 'R', 'A', 'S', 'E', 'E', 'D'};

struct AuMacEntropy {
    uint8_t state[32];
    uint32_t counter;
    unsigned credit; /* 1/64 bits */
    unsigned from_seed, from_startup, from_jitter, from_input;
    bool saved_full; /* the seed file says the threshold was reached */
    /* jitter estimator */
    uint32_t t_last, d_last, d2_last;
    uint32_t last_tick; /* tick of the last credited loop sample */
    Point mouse;
    bool have_mouse;
    /* seed file location */
    short vref;
    long dir;
    bool have_dir;
};

static void mix(AuMacEntropy *e, const void *data, size_t len)
{
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, "mix", 3);
    br_sha256_update(&c, e->state, sizeof e->state);
    uint8_t n[4] = {(uint8_t)(e->counter >> 24), (uint8_t)(e->counter >> 16), (uint8_t)(e->counter >> 8),
                    (uint8_t)e->counter};
    e->counter++;
    br_sha256_update(&c, n, sizeof n);
    br_sha256_update(&c, data, len);
    br_sha256_out(&c, e->state);
}

static void credit(AuMacEntropy *e, unsigned amount, unsigned *bucket)
{
    unsigned room = kMaxCredit - e->credit;
    if (amount > room)
        amount = room;
    e->credit += amount;
    if (bucket)
        *bucket += amount;
}

static uint32_t micros(void)
{
    AuMacWide w;
    Microseconds(&w);
    return w.lo;
}

static void mix_clocks(AuMacEntropy *e)
{
    struct {
        AuMacWide us;
        uint32_t ticks;
        unsigned long secs;
    } c;
    Microseconds(&c.us);
    c.ticks = TickCount();
    c.secs = 0;
    ReadDateTime(&c.secs);
    mix(e, &c, sizeof c);
}

/* One jitter sample: returns true when its third-order difference is not
 * zero. */
static bool jitter_sample(AuMacEntropy *e, uint32_t t)
{
    uint32_t d = t - e->t_last;
    uint32_t d2 = d - e->d_last;
    uint32_t d3 = d2 - e->d2_last;
    e->t_last = t;
    e->d_last = d;
    e->d2_last = d2;
    return d3 != 0;
}

/* ---- Seed file ---- */

static void seed_name(Str255 out)
{
    static const char name[] = "Random Seed";
    out[0] = (unsigned char)(sizeof name - 1);
    memcpy(out + 1, name, sizeof name - 1);
}

static bool find_dir(AuMacEntropy *e)
{
    short vref;
    long dir;
    /* kOnSystemDisk; create the Preferences folder if it is missing. */
    if (FindFolder(-32768, kPreferencesFolderType, true, &vref, &dir) != noErr)
        return false;
    Str255 aura = {4, 'A', 'u', 'r', 'a'};
    FSSpec spec;
    OSErr err = FSMakeFSSpec(vref, dir, aura, &spec);
    long sub = 0;
    if (err == fnfErr) {
        if (FSpDirCreate(&spec, -1, &sub) != noErr)
            return false;
    } else if (err == noErr) {
        CInfoPBRec pb;
        Str255 name;
        memcpy(name, spec.name, spec.name[0] + 1);
        memset(&pb, 0, sizeof pb);
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = spec.vRefNum;
        pb.dirInfo.ioDrDirID = spec.parID;
        pb.dirInfo.ioFDirIndex = 0;
        if (PBGetCatInfoSync(&pb) != noErr || !(pb.dirInfo.ioFlAttrib & 0x10))
            return false;
        sub = pb.dirInfo.ioDrDirID;
    } else {
        return false;
    }
    e->vref = vref;
    e->dir = sub;
    e->have_dir = true;
    return true;
}

static void read_seed(AuMacEntropy *e)
{
    if (!e->have_dir)
        return;
    Str255 name;
    seed_name(name);
    FSSpec spec;
    if (FSMakeFSSpec(e->vref, e->dir, name, &spec) != noErr)
        return;
    short ref;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr)
        return;
    uint8_t buf[sizeof kSeedMagic + kSeedLen + 1];
    long count = (long)sizeof buf;
    uint32_t t0 = micros();
    OSErr err = FSRead(ref, &count, buf);
    uint32_t t1 = micros();
    FSClose(ref);
    mix(e, &t0, sizeof t0);
    mix(e, &t1, sizeof t1);
    if ((err != noErr && err != eofErr) || count != (long)sizeof buf || memcmp(buf, kSeedMagic, sizeof kSeedMagic))
        return;
    mix(e, buf + sizeof kSeedMagic, kSeedLen);
    if (buf[sizeof kSeedMagic + kSeedLen] == 1) {
        e->saved_full = true;
        credit(e, kSeedCredit, &e->from_seed);
    }
    memset(buf, 0, sizeof buf);
}

/* Writes a seed derived from the pool (not usable to recompute any
 * output) and moves the pool on. */
static void write_seed(AuMacEntropy *e)
{
    if (!e->have_dir)
        return;
    uint8_t buf[sizeof kSeedMagic + kSeedLen + 1];
    memcpy(buf, kSeedMagic, sizeof kSeedMagic);
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, "seed", 4);
    br_sha256_update(&c, e->state, sizeof e->state);
    br_sha256_out(&c, buf + sizeof kSeedMagic);
    br_sha256_init(&c);
    br_sha256_update(&c, "next", 4);
    br_sha256_update(&c, e->state, sizeof e->state);
    br_sha256_out(&c, e->state);
    bool full = e->credit >= AU_MAC_ENTROPY_THRESHOLD * kBit;
    buf[sizeof kSeedMagic + kSeedLen] = full ? 1 : 0;

    Str255 name;
    seed_name(name);
    FSSpec spec;
    OSErr err = FSMakeFSSpec(e->vref, e->dir, name, &spec);
    if (err == fnfErr)
        err = FSpCreate(&spec, 'AURA', 'SEED', -1);
    if (err != noErr)
        return;
    short ref;
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr)
        return;
    long count = (long)sizeof buf;
    err = FSWrite(ref, &count, buf);
    if (err == noErr)
        SetEOF(ref, count);
    FSClose(ref);
    FlushVol(NULL, e->vref);
    memset(buf, 0, sizeof buf);
    if (full)
        e->saved_full = true;
}

/* ---- Public ---- */

AuMacEntropy *au_mac_entropy_new(void)
{
    AuMacEntropy *e = au_alloc(sizeof *e);
    memset(e, 0, sizeof *e);
    mix_clocks(e);
    struct {
        long free_mem, temp_mem;
        void *stack, *heap;
        Point mouse;
    } s;
    s.free_mem = FreeMem();
    s.temp_mem = TempFreeMem();
    s.stack = &s;
    s.heap = e;
    GetMouse(&s.mouse);
    mix(e, &s, sizeof s);

    if (find_dir(e))
        read_seed(e);

    /* Start-up jitter: time a small, memory-touching computation over
     * and over. */
    uint32_t samples[64];
    unsigned got = 0;
    volatile uint32_t sink = 0;
    e->t_last = micros();
    for (int i = 0; i < kStartupSamples; i++) {
        for (int k = 0; k < 16 + (i & 15); k++)
            sink = sink * 1664525u + 1013904223u + (uint32_t)k;
        uint32_t t = micros();
        if (jitter_sample(e, t) && e->from_startup < kStartupMax)
            credit(e, kStartupSample, &e->from_startup);
        samples[got++] = t ^ sink;
        if (got == 64) {
            mix(e, samples, sizeof samples);
            got = 0;
        }
    }
    if (got)
        mix(e, samples, got * sizeof samples[0]);
    mix_clocks(e);
    /* Never start from the same seed twice, even after a crash. */
    write_seed(e);
    return e;
}

void au_mac_entropy_free(AuMacEntropy *e)
{
    if (!e)
        return;
    mix_clocks(e);
    write_seed(e);
    memset(e, 0, sizeof *e);
    au_free(e);
}

void au_mac_entropy_event(AuMacEntropy *e, const EventRecord *ev)
{
    struct {
        EventRecord ev;
        AuMacWide us;
    } m;
    m.ev = *ev;
    Microseconds(&m.us);
    mix(e, &m, sizeof m);
    switch (ev->what) {
    case mouseDown:
    case mouseUp:
    case keyDown:
    case keyUp:
    case autoKey:
        credit(e, kInputCredit, &e->from_input);
        break;
    default:
        break;
    }
}

void au_mac_entropy_idle(AuMacEntropy *e)
{
    bool below = e->credit < AU_MAC_ENTROPY_THRESHOLD * kBit;
    uint32_t t = micros();
    /* Every sample is mixed; at most one a tick is credited, so a loop
     * that spins fast does not earn credit faster. */
    uint32_t tick = TickCount();
    if (jitter_sample(e, t) && tick != e->last_tick) {
        e->last_tick = tick;
        credit(e, kJitterSample, &e->from_jitter);
    }
    Point p;
    GetMouse(&p);
    struct {
        uint32_t t;
        Point p;
    } m = {t, p};
    mix(e, &m, sizeof m);
    if (e->have_mouse && (p.h != e->mouse.h || p.v != e->mouse.v))
        credit(e, kMouseCredit, &e->from_input);
    e->mouse = p;
    e->have_mouse = true;
    /* Record a full pool in the seed file as soon as there is one. */
    if (below && e->credit >= AU_MAC_ENTROPY_THRESHOLD * kBit && !e->saved_full)
        write_seed(e);
}

void au_mac_entropy_add(AuMacEntropy *e, const void *data, size_t len, unsigned amount)
{
    mix(e, data, len);
    if (amount)
        credit(e, amount, NULL);
}

bool au_mac_entropy_read(AuMacEntropy *e, void *out, size_t len)
{
    if (e->credit < AU_MAC_ENTROPY_THRESHOLD * kBit)
        return false;
    uint32_t t = micros();
    mix(e, &t, sizeof t);
    uint8_t *o = out;
    while (len) {
        uint8_t block[32];
        br_sha256_context c;
        br_sha256_init(&c);
        br_sha256_update(&c, "out", 3);
        br_sha256_update(&c, e->state, sizeof e->state);
        uint8_t n[4] = {(uint8_t)(e->counter >> 24), (uint8_t)(e->counter >> 16), (uint8_t)(e->counter >> 8),
                        (uint8_t)e->counter};
        e->counter++;
        br_sha256_update(&c, n, sizeof n);
        br_sha256_out(&c, block);
        size_t k = len < sizeof block ? len : sizeof block;
        memcpy(o, block, k);
        o += k;
        len -= k;
        memset(block, 0, sizeof block);
    }
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, "next", 4);
    br_sha256_update(&c, e->state, sizeof e->state);
    br_sha256_out(&c, e->state);
    return true;
}

unsigned au_mac_entropy_bits(const AuMacEntropy *e)
{
    return e->credit / kBit;
}

void au_mac_entropy_sources(const AuMacEntropy *e, unsigned *seed, unsigned *startup, unsigned *jitter,
                            unsigned *input)
{
    *seed = e->from_seed / kBit;
    *startup = e->from_startup / kBit;
    *jitter = e->from_jitter / kBit;
    *input = e->from_input / kBit;
}
