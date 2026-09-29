#ifndef B3_NATIVE_NAME_IMAGE_H
#define B3_NATIVE_NAME_IMAGE_H
#include "../chomp-b3-macos/name_transport.h"

/* The native app limits new names to the 16-byte hardware-tested display bound.
 * Keep the original parser's 40-byte limit so older names can still be read
 * and shortened without changing any other device settings. */
#define B3_NEW_NAME_MAX 16U

static void put32(unsigned char *p, size_t value) {
    b3_name_put16(p, value);
    b3_name_put16(p + 2, value >> 16);
}

static void checksum(unsigned char *data, size_t length) {
    unsigned value = 0;
    b3_name_put16(data + length - 12, 0);
    for (size_t i = 0; i < length; i += 2) value ^= b3_name_u16(data + i);
    b3_name_put16(data + length - 12, value);
}

/* Retain the Python parser's duplicate-PSID and record-boundary checks in all
 * three files, in addition to the portable native writer's layout guard. */
static int parse(const unsigned char *data, size_t length, b3_name_layout *layout) {
    if (!b3_name_parse(data, length, layout)) return 0;
    for (size_t file = 0; file < 3; file++) {
        unsigned char seen[8192] = {0};
        size_t offset = b3_name_u32(data + 28 + 12 * file);
        size_t end = offset + b3_name_u32(data + 32 + 12 * file);
        for (size_t cursor = offset + 8; cursor < end;) {
            if (end - cursor < 4) return 0;
            unsigned psid = b3_name_u16(data + cursor);
            size_t size = b3_name_u16(data + cursor + 2);
            size_t record = 4 + size + (size & 1);
            if (record > end - cursor || (seen[psid / 8] & (1U << (psid & 7)))) return 0;
            seen[psid / 8] |= (unsigned char)(1U << (psid & 7));
            cursor += record;
        }
    }
    return 1;
}

static size_t name_length(const unsigned char *data, const b3_name_layout *layout) {
    size_t size = data[layout->name_offset + 5];
    return size - (data[layout->name_offset + 6 + size - 1] == 0);
}

static int patch(const unsigned char *before, size_t before_length,
                 const unsigned char *name, size_t name_size,
                 unsigned char *after, size_t *after_length) {
    b3_name_layout original = {0}, updated = {0};
    if (name_size > B3_NEW_NAME_MAX || !b3_name_utf8(name, name_size) ||
        !parse(before, before_length, &original)) return 0;
    if (name_length(before, &original) == name_size &&
        !memcmp(before + original.name_offset + 6, name, name_size)) {
        memcpy(after, before, before_length);
        *after_length = before_length;
        return 1;
    }
    size_t padded = name_size + (name_size & 1);
    size_t record = 6 + padded;
    size_t bt_size = original.bt_size - original.name_size + record;
    size_t old_allocation = b3_name_align4(original.bt_size);
    size_t new_allocation = b3_name_align4(bt_size);
    size_t length = before_length - old_allocation + new_allocation;
    if (length > B3_NAME_IMAGE_MAX) return 0;

    memcpy(after, before, original.name_offset);
    unsigned char *replacement = after + original.name_offset;
    b3_name_put16(replacement, 0x0108);
    b3_name_put16(replacement + 2, padded + 2);
    replacement[4] = 0xa1;
    replacement[5] = (unsigned char)padded;
    memcpy(replacement + 6, name, name_size);
    if (padded != name_size) replacement[6 + name_size] = 0;
    size_t suffix = original.bt_offset + original.bt_size - original.name_offset - original.name_size;
    memcpy(replacement + record, before + original.name_offset + original.name_size, suffix);
    memset(after + original.bt_offset + bt_size, 0, new_allocation - bt_size);
    size_t old_tail = original.bt_offset + old_allocation;
    size_t new_tail = original.bt_offset + new_allocation;
    memcpy(after + new_tail, before + old_tail, before_length - old_tail);

    put32(after + 44, bt_size);
    put32(after + 52, original.audio_offset - old_allocation + new_allocation);
    put32(after + 4, length);
    put32(after + length - 8, length);
    checksum(after, length);
    if (!parse(after, length, &updated) ||
        !b3_name_only_change(before, before_length, after, length) ||
        name_length(after, &updated) != name_size ||
        memcmp(after + updated.name_offset + 6, name, name_size)) return 0;
    *after_length = length;
    return 1;
}

#endif
