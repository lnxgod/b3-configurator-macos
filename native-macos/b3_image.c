/* Offline B3 name-image utility. No USB operations or Python runtime required. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../native-common/name_image.h"

static int read_image(const char *path, unsigned char *data, size_t *length) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size > B3_NAME_IMAGE_MAX) {
        close(fd);
        errno = EINVAL;
        return 0;
    }
    size_t count = 0, expected = (size_t)info.st_size;
    while (count < expected) {
        ssize_t received = read(fd, data + count, expected - count);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) {
            int saved = received < 0 ? errno : EIO;
            close(fd);
            errno = saved;
            return 0;
        }
        count += (size_t)received;
    }
    unsigned char extra;
    ssize_t trailing;
    do { trailing = read(fd, &extra, 1); } while (trailing < 0 && errno == EINTR);
    int saved = trailing < 0 ? errno : EINVAL;
    if (close(fd) && trailing == 0) return 0;
    if (trailing != 0) { errno = saved; return 0; }
    *length = count;
    return 1;
}

static int write_private(const char *path, const unsigned char *data, size_t length) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return 0;
    FILE *output = fdopen(fd, "wb");
    if (!output) {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return 0;
    }
    int saved = 0;
    if (fchmod(fd, 0600) || fwrite(data, 1, length, output) != length ||
        fflush(output) || fsync(fd)) saved = errno ? errno : EIO;
    if (fclose(output) && !saved) saved = errno;
    if (saved) {
        unlink(path);
        errno = saved;
        return 0;
    }
    return 1;
}

/* Synthetic fixture includes unrelated records before and after the name and
 * nonempty system/audio files so preservation checks exercise real movement. */
static size_t fixture_with_name(unsigned char *data, size_t audio_payload,
                                const unsigned char *device_name, size_t name_size) {
    static const char *const files[] = {
        "subsys0_config3.hcf", "subsys1_config2.hcf", "subsys7_config2.hcf"
    };
    const unsigned layers[] = {3, 0x12, 0x72};
    size_t padded = name_size + (name_size & 1);
    size_t sizes[] = {24, 30 + padded, 12 + audio_payload};
    memset(data, 0, B3_NAME_IMAGE_MAX);
    memcpy(data, "File", 4);
    put32(data + 8, 4);
    put32(data + 12, 0x80000000U);
    put32(data + 16, 2);
    put32(data + 20, 3);
    size_t cursor = 128;
    for (size_t i = 0; i < 3; i++) {
        size_t entry = 24 + 12 * i, name = 62 + 22 * i;
        put32(data + entry, name);
        put32(data + entry + 4, cursor);
        put32(data + entry + 8, sizes[i]);
        b3_name_put16(data + name, 19);
        memcpy(data + name + 2, files[i], 19);
        data[cursor + 6] = (unsigned char)layers[i];
        data[cursor + 7] = 1;
        if (i == 0) {
            b3_name_put16(data + cursor + 8, 0x20);
            b3_name_put16(data + cursor + 10, 4);
            memcpy(data + cursor + 12, "SYS1", 4);
            b3_name_put16(data + cursor + 16, 0x21);
            b3_name_put16(data + cursor + 18, 4);
            memcpy(data + cursor + 20, "SYS2", 4);
        } else if (i == 1) {
            b3_name_put16(data + cursor + 8, 0x101);
            b3_name_put16(data + cursor + 10, 4);
            memcpy(data + cursor + 12, "PRE!", 4);
            b3_name_put16(data + cursor + 16, 0x108);
            b3_name_put16(data + cursor + 18, padded + 2);
            data[cursor + 20] = 0xa1;
            data[cursor + 21] = (unsigned char)padded;
            memcpy(data + cursor + 22, device_name, name_size);
            b3_name_put16(data + cursor + 22 + padded, 0x109);
            b3_name_put16(data + cursor + 24 + padded, 3);
            memcpy(data + cursor + 26 + padded, "END", 3);
        } else {
            b3_name_put16(data + cursor + 8, 0x701);
            b3_name_put16(data + cursor + 10, audio_payload);
            for (size_t k = 0; k < audio_payload; k++) data[cursor + 12 + k] = (unsigned char)(k * 17 + 3);
        }
        cursor += b3_name_align4(sizes[i]);
    }
    size_t length = cursor + 24;
    put32(data + 4, length);
    b3_name_put16(data + length - 10, 0x101);
    put32(data + length - 8, length);
    memcpy(data + length - 4, "File", 4);
    checksum(data, length);
    return length;
}

static size_t fixture(unsigned char *data, size_t audio_payload) {
    return fixture_with_name(data, audio_payload, (const unsigned char *)"CHOMP", 5);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Self-test failed at line %d: %s\n", __LINE__, #condition); return 0; \
} } while (0)

static int self_test(void) {
    unsigned char before[B3_NAME_IMAGE_MAX], after[B3_NAME_IMAGE_MAX], scratch[B3_NAME_IMAGE_MAX];
    size_t length = fixture(before, 16), changed = 0, restored = 0;
    b3_name_layout layout = {0};
    CHECK(parse(before, length, &layout));
    CHECK(name_length(before, &layout) == 5);
    CHECK(patch(before, length, (const unsigned char *)"CHOMP", 5, after, &changed));
    CHECK(changed == length && !memcmp(before, after, length));

    const size_t boundaries[] = {1, 2, 3, 4, 15, 16};
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        unsigned char name[B3_NEW_NAME_MAX];
        memset(name, 'X', sizeof(name));
        CHECK(patch(before, length, name, boundaries[i], after, &changed));
        CHECK(b3_name_only_change(before, length, after, changed));
        CHECK(patch(after, changed, (const unsigned char *)"CHOMP", 5, scratch, &restored));
        CHECK(restored == length && !memcmp(before, scratch, length));
    }
    const unsigned char observed_name[] = "GameChangersAIh1";
    CHECK(sizeof(observed_name) - 1 == B3_NEW_NAME_MAX);
    CHECK(patch(before, length, observed_name, sizeof(observed_name) - 1, after, &changed));
    CHECK(parse(after, changed, &layout) && name_length(after, &layout) == B3_NEW_NAME_MAX);
    CHECK(!memcmp(after + layout.name_offset + 6, observed_name, B3_NEW_NAME_MAX));
    unsigned char emoji[B3_NEW_NAME_MAX];
    for (size_t i = 0; i < sizeof(emoji); i += 4) memcpy(emoji + i, "\xf0\x9f\x8e\xb5", 4);
    CHECK(patch(before, length, emoji, sizeof(emoji), after, &changed));
    const char *invalid[] = {"", "a\n", "\xc2\x85", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!patch(before, length, (const unsigned char *)invalid[i], strlen(invalid[i]), after, &changed));
    unsigned char long_name[17];
    memset(long_name, 'A', sizeof(long_name));
    CHECK(!patch(before, length, long_name, sizeof(long_name), after, &changed));
    unsigned char long_emoji[20];
    for (size_t i = 0; i < sizeof(long_emoji); i += 4) memcpy(long_emoji + i, "\xf0\x9f\x8e\xb5", 4);
    CHECK(!patch(before, length, long_emoji, sizeof(long_emoji), after, &changed));

    /* Reading a legacy name remains valid even though it cannot be a new target. */
    unsigned char legacy_name[40];
    memset(legacy_name, 'L', sizeof(legacy_name));
    size_t legacy_length = fixture_with_name(scratch, 16, legacy_name, sizeof(legacy_name));
    CHECK(parse(scratch, legacy_length, &layout));
    CHECK(name_length(scratch, &layout) == sizeof(legacy_name));
    CHECK(!memcmp(scratch + layout.name_offset + 6, legacy_name, sizeof(legacy_name)));
    CHECK(!patch(scratch, legacy_length, legacy_name, sizeof(legacy_name), after, &changed));
    CHECK(patch(scratch, legacy_length, (const unsigned char *)"CHOMP", 5, after, &changed));
    CHECK(b3_name_only_change(scratch, legacy_length, after, changed));
    CHECK(changed == length && !memcmp(after, before, length));

    memcpy(scratch, before, length);
    scratch[140] ^= 1;
    CHECK(!parse(scratch, length, &layout));
    checksum(scratch, length);
    CHECK(parse(scratch, length, &layout));
    CHECK(!b3_name_only_change(before, length, scratch, length));
    memcpy(scratch, before, length);
    b3_name_put16(scratch + 144, 0x20); /* Duplicate system-file PSID. */
    checksum(scratch, length);
    CHECK(!parse(scratch, length, &layout));
    memcpy(scratch, before, length);
    b3_name_put16(scratch + length - 10, 0x102);
    checksum(scratch, length);
    CHECK(!parse(scratch, length, &layout));
    memcpy(scratch, before, length);
    put32(scratch + 52, b3_name_u32(scratch + 52) + 4);
    checksum(scratch, length);
    CHECK(!parse(scratch, length, &layout));
    CHECK(!parse(before, length - 1, &layout));

    size_t maximum = fixture(scratch, 1820);
    CHECK(maximum == B3_NAME_IMAGE_MAX && parse(scratch, maximum, &layout));
    CHECK(!patch(scratch, maximum, emoji, sizeof(emoji), after, &changed));
    CHECK(patch(scratch, maximum, (const unsigned char *)"A", 1, after, &changed));

    char directory[] = "/tmp/b3-image-test.XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256], link[256];
    CHECK(snprintf(path, sizeof(path), "%s/image.bin", directory) > 0);
    CHECK(snprintf(link, sizeof(link), "%s/link.bin", directory) > 0);
    CHECK(write_private(path, before, length));
    struct stat info;
    CHECK(!stat(path, &info) && (info.st_mode & 0777) == 0600);
    CHECK(!write_private(path, after, changed) && errno == EEXIST);
    CHECK(read_image(path, scratch, &restored) && restored == length && !memcmp(before, scratch, length));
    CHECK(!symlink(path, link));
    CHECK(!write_private(link, before, length));
    CHECK(!read_image(link, scratch, &restored));
    CHECK(!unlink(link) && !unlink(path) && !rmdir(directory));
    puts("B3 image self-test passed (16-byte names, legacy-name shortening, UTF-8, bounds, preservation, corruption, private files).");
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test")) return self_test() ? 0 : 1;
    int reading = argc == 3 && !strcmp(argv[1], "--read-name");
    int patching = argc == 5 && !strcmp(argv[1], "--patch-name");
    if (!reading && !patching) {
        fprintf(stderr, "Usage: %s --read-name INPUT\n       %s --patch-name INPUT OUTPUT NAME\n       %s --self-test\n", argv[0], argv[0], argv[0]);
        return 2;
    }
    unsigned char before[B3_NAME_IMAGE_MAX], after[B3_NAME_IMAGE_MAX];
    size_t length = 0, changed = 0;
    b3_name_layout layout = {0};
    if (!read_image(argv[2], before, &length)) {
        perror("Cannot read configuration image");
        return 1;
    }
    if (!parse(before, length, &layout)) {
        fputs("Unsupported or invalid B3 configuration image.\n", stderr);
        return 1;
    }
    if (reading) {
        size_t size = name_length(before, &layout);
        if (fwrite(before + layout.name_offset + 6, 1, size, stdout) != size ||
            putchar('\n') == EOF || fflush(stdout)) return 1;
        return 0;
    }
    if (!patch(before, length, (const unsigned char *)argv[4], strlen(argv[4]), after, &changed)) {
        fputs("Cannot prepare name image: use 1-16 UTF-8 bytes without control characters and a supported image within the device size limit.\n", stderr);
        return 1;
    }
    if (!write_private(argv[3], after, changed)) {
        perror("Cannot create private name image (output must not exist)");
        return 1;
    }
    return 0;
}
