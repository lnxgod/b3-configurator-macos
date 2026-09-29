#ifndef B3_PIN_GUARD_H
#define B3_PIN_GUARD_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Schema MD5 5819ff8235a6d18029d414ec778a7f5c, block 133, 25 BE words.
 * Only the four ASCII PIN digits and Word 25 bit 11 may change. */
static inline int b3_pin_guard(const uint8_t *before, size_t before_size,
                               const uint8_t *after, size_t after_size) {
    if (!before || !after || before_size != 50 || after_size != 50) return 0;
    for (size_t i = 0; i < 50; ++i) {
        if (i >= 36 && i <= 39) {
            if (after[i] < '0' || after[i] > '9') return 0;
        } else if (i == 48) {
            if ((before[i] ^ after[i]) & (uint8_t)~0x08) return 0;
        } else if (before[i] != after[i]) return 0;
    }
    return 1;
}
static inline int b3_pin_build_report(const uint8_t before[50],
                                      const uint8_t after[50], uint8_t report[66]) {
    if (!report || !b3_pin_guard(before, 50, after, 50)) return 0;
    const uint8_t prefix[] = {1, 58, 7, 0, 0, 54, 0, 133, 0, 50};
    memset(report, 0, 66);
    memcpy(report, prefix, sizeof(prefix));
    memcpy(report + sizeof(prefix), after, 50);
    return 1;
}
#endif
