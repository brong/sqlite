/* One-shot XXH3 over one buffer, against the streaming API over two --
 * which is the difference between how format 2 and format 3 checksum the
 * same bytes at the end of a merge.  Format 2's records region was one
 * contiguous buffer; format 3's keys and values live in separate buffers
 * (deliberately, so the values are written from where they were
 * accumulated), so its trailer checksum has to stream.
 *
 * The digests are XORed into one sink and printed: the two paths hash the
 * same byte sequence, so a sink of 0 is the check that this is comparing
 * like with like rather than two different workloads.
 *
 *   cc -O2 -Iext/zeroskip -o /tmp/xxhcmp test/zs/xxhcmp.c && /tmp/xxhcmp
 *
 * Take the LAST pass: the first two are hashing a cold buffer.  On an M-series
 * laptop it settles at one-shot ~51 GB/s against ~21 GB/s streamed. */
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    /* 21MB total, split the way a 2M-record/100-byte load splits it:
     * a keys region about a tenth the size of the values region. */
    size_t total = (argc > 1) ? (size_t)atol(argv[1]) : 21u << 20;
    size_t klen = total / 11, vlen = total - klen;
    char *one = malloc(total);
    char *k = one, *v = malloc(vlen);
    int reps = 20;

    for (size_t i = 0; i < total; i++) one[i] = (char)(i * 7);
    memcpy(v, one + klen, vlen);

    for (int pass = 0; pass < 3; pass++) {
        double t0, oneshot, streamed;
        uint64_t sink = 0;

        t0 = now();
        for (int r = 0; r < reps; r++) sink ^= XXH3_64bits(one, total);
        oneshot = now() - t0;

        t0 = now();
        for (int r = 0; r < reps; r++) {
            XXH3_state_t *s = XXH3_createState();
            XXH3_64bits_reset(s);
            XXH3_64bits_update(s, k, klen);
            XXH3_64bits_update(s, v, vlen);
            sink ^= XXH3_64bits_digest(s);
            XXH3_freeState(s);
        }
        streamed = now() - t0;

        printf("pass %d  one-shot %6.1f MB/s   streamed(2 buffers) %6.1f MB/s"
               "   ratio %.2fx   [%llx]\n",
               pass,
               total * reps / oneshot / 1e6,
               total * reps / streamed / 1e6,
               oneshot / streamed,
               (unsigned long long)sink);
    }
    return 0;
}
