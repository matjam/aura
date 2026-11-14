/* Shared helpers for the paint tests. */
#ifndef AURA_TEST_PAINT_H
#define AURA_TEST_PAINT_H

#include "paint/font.h"
#include "paint/paint.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define CHECK_NEAR(a, b, tol) \
    do { \
        double a_ = (a), b_ = (b); \
        if (!(a_ - b_ <= (tol) && b_ - a_ <= (tol))) { \
            fprintf(stderr, "%s:%d: %s = %g, expected %g (+-%g)\n", __FILE__, __LINE__, #a, a_, b_, \
                    (double)(tol)); \
            exit(1); \
        } \
    } while (0)

void test_fonts(void);

#endif
