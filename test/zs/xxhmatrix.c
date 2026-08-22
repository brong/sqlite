/* xxhash under the flags zeroskip ACTUALLY builds it with.
 *
 * zeroskip.c defines XXH_NO_INLINE_HINTS 1 before including xxhash.h -- for a
 * real reason on x86 ("callers without the SSE2 target attribute cannot inline
 * always_inline SSE2 helpers") -- which turns XXH_FORCE_INLINE into a plain
 * static.  An earlier version of this measurement omitted that define, so it
 * was not measuring the library's configuration at all.
 *
 * Build four ways and the interaction is the answer:
 *   cc -O2 -Iext/zeroskip -o m00 xxhmatrix.c
 *   cc -O2 -Iext/zeroskip -DHINTS_OFF=1 -o m10 xxhmatrix.c
 *   cc -O2 -Iext/zeroskip -DXXH3_STREAM_USE_STACK=1 -o m01 xxhmatrix.c
 *   cc -O2 -Iext/zeroskip -DHINTS_OFF=1 -DXXH3_STREAM_USE_STACK=1 -o m11 ...
 */
#define XXH_INLINE_ALL
#ifdef HINTS_OFF
#define XXH_NO_INLINE_HINTS 1          /* what zeroskip.c does */
#endif
#include "xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    size_t total = (argc > 1) ? (size_t)atol(argv[1]) : 21u << 20;
    int reps = 20;
    char *buf = malloc(total);
    for (size_t i = 0; i < total; i++) buf[i] = (char)(i * 7);

    for (int pass = 0; pass < 3; pass++) {
        double t0, a, b;
        uint64_t d1 = 0, d2 = 0;

        t0 = now();
        for (int r = 0; r < reps; r++) { buf[r] = (char)r; d1 ^= XXH3_64bits(buf, total); }
        a = now() - t0;

        t0 = now();
        for (int r = 0; r < reps; r++) {
            XXH3_state_t st;
            buf[r] = (char)r;
            XXH3_64bits_reset(&st);
            /* 256KB at a time: the window zeroskip streams a big region through */
            for (size_t o = 0; o < total; o += 256u << 10) {
                size_t n = total - o < (256u << 10) ? total - o : (256u << 10);
                XXH3_64bits_update(&st, buf + o, n);
            }
            d2 ^= XXH3_64bits_digest(&st);
        }
        b = now() - t0;

        if (pass < 2) continue;
        printf("  NO_INLINE_HINTS=%d STREAM_USE_STACK=%d :"
               "  one-shot %6.1f GB/s   streamed %6.1f GB/s   %.2fx   %s\n",
#ifdef HINTS_OFF
               1,
#else
               0,
#endif
#if defined(XXH3_STREAM_USE_STACK) && XXH3_STREAM_USE_STACK >= 1
               1,
#else
               0,
#endif
               total * reps / a / 1e9, total * reps / b / 1e9, a / b,
               d1 == d2 ? "digests agree" : "DIGESTS DISAGREE");
    }
    return 0;
}
