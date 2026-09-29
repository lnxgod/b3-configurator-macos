#ifndef B3_NAME_TRANSPORT_H
#define B3_NAME_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Restricted to the observed B3 v0x0101 flat EEPROM filesystem.  The supported
 * operation changes only bt2 PSID 0x0108 and the sizes/offsets/checksum required
 * by that replacement. No unguarded write entry point is provided.
 *
 * Format evidence: HydIsp 0x180028de0/0x180024b10 (filesystem/XOR) and
 * HydBinary 0x18000b250/0x180018000 (HCF/string), x86/x64 DLLs. */
#define B3_NAME_IMAGE_MAX 2044U
#define B3_NAME_FRAME_MAX 4096U

static inline unsigned b3_name_u16(const unsigned char *p) {
    return p[0] | (unsigned)p[1] << 8;
}
static inline uint32_t b3_name_u32(const unsigned char *p) {
    return (uint32_t)b3_name_u16(p) | (uint32_t)b3_name_u16(p + 2) << 16;
}
static inline void b3_name_put16(unsigned char *p, size_t n) {
    p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8);
}
static inline size_t b3_name_align4(size_t n) { return (n + 3) & ~(size_t)3; }
static inline int b3_name_zero(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}
static inline int b3_name_utf8(const unsigned char *p, size_t n) {
    if (!n || n > 40) return 0;
    for (size_t i = 0; i < n;) {
        unsigned c = p[i++], count = 0, minimum = 0;
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { count = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; minimum = 0x800; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; minimum = 0x10000; c &= 7; }
        else return 0;
        if (count > n - i) return 0;
        for (unsigned j = 0; j < count; j++) {
            unsigned continuation = p[i++];
            if ((continuation & 0xc0) != 0x80) return 0;
            c = c << 6 | (continuation & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            (c >= 0x80 && c <= 0x9f)) return 0;
    }
    return 1;
}

typedef struct {
    size_t bt_offset, bt_size, audio_offset, audio_size;
    size_t name_offset, name_size;
} b3_name_layout;

static inline int b3_name_parse(const unsigned char *p, size_t n, b3_name_layout *v) {
    static const char *const names[] = {
        "subsys0_config3.hcf", "subsys1_config2.hcf", "subsys7_config2.hcf"
    };
    static const unsigned char bt_header[] = {0,0,0,0,0,0,0x12,1};
    if (!p || !v || n < 160 || n > B3_NAME_IMAGE_MAX || n % 4 ||
        memcmp(p, "File", 4) || memcmp(p + n - 4, "File", 4) ||
        b3_name_u32(p + 4) != n || b3_name_u32(p + n - 8) != n ||
        b3_name_u32(p + 8) != 4 || b3_name_u16(p + n - 10) != 0x101 ||
        !b3_name_zero(p + n - 24, 12)) return 0;
    unsigned checksum = 0;
    for (size_t i = 0; i < n; i += 2) checksum ^= b3_name_u16(p + i);
    if (checksum || b3_name_u32(p + 12) != 0x80000000U ||
        b3_name_u32(p + 16) != 2 || b3_name_u32(p + 20) != 3 ||
        !b3_name_zero(p + 60, 2)) return 0;
    size_t cursor = 128;
    for (size_t i = 0; i < 3; i++) {
        size_t entry = 24 + 12*i, name = 62 + 22*i;
        size_t size = b3_name_u32(p + entry + 8);
        if (b3_name_u32(p + entry) != name || b3_name_u16(p + name) != 19 ||
            memcmp(p + name + 2, names[i], 19) || p[name + 21] ||
            b3_name_u32(p + entry + 4) != cursor || cursor > n - 24 ||
            size > n - 24 - cursor) return 0;
        size_t aligned = b3_name_align4(size);
        if (aligned > n - 24 - cursor || !b3_name_zero(p + cursor + size, aligned - size)) return 0;
        const unsigned subsystem_layer[] = {3, 0x12, 0x72};
        if (size < 8 || !b3_name_zero(p + cursor, 6) ||
            p[cursor + 6] != subsystem_layer[i] || p[cursor + 7] != 1) return 0;
        if (i == 1) { v->bt_offset = cursor; v->bt_size = size; }
        if (i == 2) { v->audio_offset = cursor; v->audio_size = size; }
        cursor += aligned;
    }
    if (cursor != n - 24 || v->bt_size < 8 ||
        memcmp(p + v->bt_offset, bt_header, sizeof(bt_header))) return 0;
    size_t end = v->bt_offset + v->bt_size;
    cursor = v->bt_offset + 8;
    v->name_offset = v->name_size = 0;
    while (cursor < end) {
        if (end - cursor < 4) return 0;
        size_t length = b3_name_u16(p + cursor + 2), record = 4 + length + (length & 1);
        if (record > end - cursor || ((length & 1) && p[cursor + 4 + length])) return 0;
        if (b3_name_u16(p + cursor) == 0x108) {
            if (v->name_size || length < 4 || p[cursor + 4] != 0xa1) return 0;
            size_t padded = p[cursor + 5];
            if (padded < 2 || padded > 40 || (padded & 1) || length != padded + 2) return 0;
            size_t raw = padded - (p[cursor + 6 + padded - 1] == 0);
            if (!b3_name_utf8(p + cursor + 6, raw)) return 0;
            v->name_offset = cursor; v->name_size = record;
        }
        cursor += record;
    }
    return v->name_size != 0;
}

static inline int b3_name_only_change(const unsigned char *a, size_t an,
                                      const unsigned char *b, size_t bn) {
    b3_name_layout av = {0}, bv = {0};
    if (!b3_name_parse(a, an, &av) || !b3_name_parse(b, bn, &bv)) return 0;
    /* Only total size, BT file size and subsequent file offset may differ in
     * the fixed prefix. Both parsed images independently validate these. */
    for (size_t i = 0; i < 128; i++) {
        if ((i >= 4 && i < 8) || (i >= 44 && i < 48) || (i >= 52 && i < 56)) continue;
        if (a[i] != b[i]) return 0;
    }
    if (av.bt_offset != bv.bt_offset || av.audio_size != bv.audio_size ||
        memcmp(a + 128, b + 128, av.bt_offset - 128)) return 0;
    size_t aprefix = av.name_offset - av.bt_offset, bprefix = bv.name_offset - bv.bt_offset;
    size_t asuffix = av.bt_size - aprefix - av.name_size;
    size_t bsuffix = bv.bt_size - bprefix - bv.name_size;
    if (aprefix != bprefix || asuffix != bsuffix ||
        memcmp(a + av.bt_offset, b + bv.bt_offset, aprefix) ||
        memcmp(a + av.name_offset + av.name_size, b + bv.name_offset + bv.name_size, asuffix) ||
        memcmp(a + av.audio_offset, b + bv.audio_offset, av.audio_size)) return 0;
    return 1;
}

typedef int (*b3_name_exchange)(void *, const unsigned char *, size_t,
                              unsigned char *, size_t *, unsigned);
enum b3_name_result {
    B3_NAME_OK = 0, B3_NAME_GUARD = 1, B3_NAME_READ = 2,
    B3_NAME_STALE = 3, B3_NAME_WRITE_IO = 4, B3_NAME_WRITE_REPLY = 5,
    B3_NAME_VERIFY_READ = 6, B3_NAME_VERIFY_MISMATCH = 7
};

/* Raw USB/ISP frame -> the payload following the local command. The vendor
 * PtCmdReceive (0x180023060) checks length/status and PtCmdLocalReceive
 * (0x180023e10) checks the local opcode. Tag bytes are deliberately ignored.
 * UsbDebug.sys's receive limit is strictly less than 4096 bytes. */
static inline int b3_name_response(const unsigned char *p, size_t n, unsigned local,
                                   const unsigned char **data, size_t *length) {
    static const unsigned char prefix[] = {3,0,0x91,0x28,9,0};
    if (!p || !data || !length || n < 17 || n >= B3_NAME_FRAME_MAX ||
        memcmp(p, prefix, sizeof(prefix))) return 0;
    size_t padding = b3_name_u16(p + 6) & 3;
    size_t pdu = n - 8 - padding;
    if (pdu < 9 || b3_name_u16(p + 8) != 3 ||
        b3_name_u16(p + 10) != pdu - 8 || b3_name_u16(p + 14) || p[16] != local) return 0;
    *data = p + 17; *length = pdu - 9;
    return 1;
}

static inline int b3_name_read(b3_name_exchange exchange, void *context,
                               unsigned char *image, size_t *image_length) {
    static const unsigned char read_request[] = {
        3,0,0x24,0x2a,9,0,3,0,3,0,1,0,0x34,0x12,0,0,4,0xfa,0xfa,0xfa
    };
    unsigned char response[B3_NAME_FRAME_MAX];
    size_t received = sizeof(response), length = 0;
    const unsigned char *data = NULL;
    if (exchange(context, read_request, sizeof(read_request), response, &received, 5000) ||
        !b3_name_response(response, received, 4, &data, &length) ||
        length > B3_NAME_IMAGE_MAX || (length & 1)) return 0;
    memcpy(image, data, length); *image_length = length;
    return 1;
}

/* Caller owns an already-connected PTCMD tunnel and MUST disconnect afterward,
 * including on errors. Each exchange does exactly one OUT (with terminating
 * USB ZLP when needed) and one bounded IN. This helper never retries or resets.
 * HydProtocols::WriteFileSystem 0x18002a5e0 accepts an even image <0x7fd bytes,
 * sends production3/local5 once and waits 5000 ms. The callback must not log
 * payload bytes, which may contain keys. write_attempted is set BEFORE OUT;
 * a timeout or rejected response does not establish that nothing was written. */
static inline int b3_name_write(b3_name_exchange exchange, void *context,
                                const unsigned char *before, size_t before_len,
                                const unsigned char *after, size_t after_len,
                                int *write_attempted) {
    if (write_attempted) *write_attempted = 0;
    if (!write_attempted || !exchange ||
        !b3_name_only_change(before, before_len, after, after_len)) return B3_NAME_GUARD;
    unsigned char current[B3_NAME_IMAGE_MAX];
    size_t current_length = 0;
    if (!b3_name_read(exchange, context, current, &current_length)) return B3_NAME_READ;
    if (current_length != before_len || memcmp(current, before, before_len)) return B3_NAME_STALE;
    if (before_len == after_len && !memcmp(before, after, before_len)) return B3_NAME_OK;

    unsigned char request[B3_NAME_IMAGE_MAX + 20] = {
        3,0,0x24,0x2a,9,0,0,0,3,0,0,0,0x34,0x12,0,0,5
    };
    size_t padding = (4 - ((17 + after_len) & 3)) & 3;
    b3_name_put16(request + 6, padding);
    b3_name_put16(request + 10, after_len + 1);
    memcpy(request + 17, after, after_len);
    memset(request + 17 + after_len, 0xfa, padding);
    unsigned char response[B3_NAME_FRAME_MAX];
    size_t received = sizeof(response), ignored_length = 0;
    const unsigned char *ignored_data = NULL;
    *write_attempted = 1;
    if (exchange(context, request, 17 + after_len + padding, response, &received, 5000)) return B3_NAME_WRITE_IO;
    if (!b3_name_response(response, received, 5, &ignored_data, &ignored_length)) return B3_NAME_WRITE_REPLY;
    if (!b3_name_read(exchange, context, current, &current_length)) return B3_NAME_VERIFY_READ;
    if (current_length != after_len || memcmp(current, after, after_len)) return B3_NAME_VERIFY_MISMATCH;
    return B3_NAME_OK;
}
#endif
