/* B3 diagnostics and narrowly guarded PIN updates using IOKit and AHI report 1/2.
 * Mode switches reboot the device. PIN writes require matching firmware schema,
 * configuration mode, and an unchanged full-block baseline. No events or DFU.
 * Reference: github.com/KunYi/adk63_sink/apps/libs/ahi/ahi_protocol.h
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pin_guard.h"

static unsigned expected, response_status;
static int received, malformed, schema_done;
static uint8_t response[60];
static size_t response_length, schema_bytes, schema_expected;
static FILE *schema;
static uint8_t block_data[65535];
static size_t block_bytes, block_expected;
static unsigned requested_block;
static int block_started, block_done;
static int quiet_payloads, pin_write_authorized;
static uint8_t pin_before[50], pin_after[50];

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

static void input_report(void *context, IOReturn result, void *sender,
                         IOHIDReportType type, uint32_t id,
                         uint8_t *report, CFIndex length) {
    (void)context; (void)sender; (void)type;
    if (result != kIOReturnSuccess || id != 2) return;
    if (length < 6 || report[0] != 2 || report[1] < 4 ||
        report[1] > 64 || report[1] > length - 2) { malformed = 1; return; }
    const uint8_t *msg = report + 2, *payload = msg + 4;
    size_t size = be16(msg + 2);
    if (size != (size_t)report[1] - 4 || size > sizeof(response)) {
        malformed = 1; return;
    }
    if (msg[0] == 0x15) {
        if (!schema || schema_bytes + size > schema_expected ||
            fwrite(payload, 1, size, schema) != size) { malformed = 1; return; }
        schema_bytes += size;
        if (!(msg[1] & 1)) schema_done = 1;
        return;
    }
    printf("RX opcode=0x%02x flags=0x%02x", msg[0], msg[1]);
    if (!quiet_payloads) {
        fputs(" payload=", stdout);
        for (size_t i = 0; i < size; i++) printf("%02x", payload[i]);
    }
    putchar('\n');
    if (msg[0] == expected) {
        memcpy(response, payload, size);
        response_length = size;
        response_status = size >= 2 ? be16(payload) : 0xffff;
        received = 1;
        if (msg[0] == 8) {
            if (size != 4 || be16(payload) != 133 || msg[1]) {
                malformed = 1; return;
            }
            response_status = be16(payload + 2);
        }
        if (msg[0] == 6) {
            if (size < 6 || be16(payload) != requested_block) {
                malformed = 1; return;
            }
            response_status = be16(payload + 2);
            if (response_status != 0) { block_done = 1; return; }
            unsigned total = be16(payload + 4);
            if (!block_started) { block_expected = total; block_started = 1; }
            if (total != block_expected || block_bytes + size - 6 > block_expected) {
                malformed = 1; return;
            }
            memcpy(block_data + block_bytes, payload + 6, size - 6);
            block_bytes += size - 6;
            if (!(msg[1] & 1)) block_done = 1;
        }
        if (msg[0] == 0x0e && size == 6 && response_status == 0) {
            schema_expected = be32(payload + 2);
            if (schema_expected > 2 * 1024 * 1024) malformed = 1;
            if (!schema_expected) schema_done = 1;
        }
    }
}

static int query(IOHIDDeviceRef device, uint8_t opcode,
                 const uint8_t *payload, size_t size) {
    /* The sole write opcode is armed once after all live checks below. */
    if (!(opcode == 0x17 || opcode == 0x19 || opcode == 1 || opcode == 9 ||
          opcode == 0x1d || opcode == 0x1f || opcode == 0x13 ||
          opcode == 0x0d || opcode == 5 || opcode == 0x0b || opcode == 7) || size > 60) return 0;
    if (opcode == 0x0b && (size != 2 || payload[0] ||
        (payload[1] != 1 && payload[1] != 2))) return 0;
    uint8_t request[66] = {1, (uint8_t)(4 + size), opcode, 0, 0, (uint8_t)size};
    if (opcode == 7) {
        if (!pin_write_authorized || payload || size) return 0;
        pin_write_authorized = 0; /* Never retry an uncertain persistent write. */
        if (!b3_pin_build_report(pin_before, pin_after, request)) return 0;
    } else if (size) memcpy(request + 6, payload, size);
    expected = opcode + 1;
    received = 0; response_length = 0; response_status = 0xffff;
    IOReturn r = IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput,
                                     1, request, sizeof(request));
    printf("TX opcode=0x%02x: 0x%08x (%s)\n", opcode, r, mach_error_string(r));
    if (r != kIOReturnSuccess) return 0;
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 3;
    while (!received && (!malformed || opcode == 0x19) && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    if (!received) puts("No matching response within 3 seconds.");
    return received && (!malformed || opcode == 0x19) &&
        (response_status == 0 || (opcode == 0x0b && response_status == 1));
}

static void match_number(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}

static int read_block(IOHIDDeviceRef device, unsigned block) {
    uint8_t id[2] = {(uint8_t)(block >> 8), (uint8_t)block};
    requested_block = block;
    block_bytes = block_expected = 0;
    block_started = block_done = 0;
    memset(block_data, 0, sizeof(block_data));
    int ok = query(device, 5, id, sizeof(id));
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 10;
    while (ok && !block_done && !malformed && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    printf("Config block %u status=0x%04x bytes=%zu/%zu final=%d\n",
           block, response_status, block_bytes, block_expected, block_done);
    return ok && !malformed && response_status == 0 && block_done && block_bytes == block_expected;
}

static int read_pin_file(const char *path, uint8_t data[50]) {
    FILE *input = fopen(path, "rb");
    if (!input) { perror(path); return 0; }
    size_t size = fread(data, 1, 50, input);
    int extra = fgetc(input), ok = size == 50 && extra == EOF && !ferror(input);
    if (fclose(input) != 0) ok = 0;
    if (!ok) fputs("PIN block input must be exactly 50 bytes.\n", stderr);
    return ok;
}

int main(int argc, char **argv) {
    /* Optional: read one identified config block instead of downloading schema. */
    long block = -1, mode = -1;
    int info_only = 0, write_pin = 0;
    if (argc == 4 && !strcmp(argv[1], "--write-pin-block")) {
        /* These checks must finish before any HID enumeration or open. */
        if (!read_pin_file(argv[2], pin_before) || !read_pin_file(argv[3], pin_after) ||
            !b3_pin_guard(pin_before, sizeof(pin_before), pin_after, sizeof(pin_after))) {
            fputs("Rejected PIN update: only four ASCII digits and the PIN-mode bit may change.\n", stderr);
            return 2;
        }
        write_pin = quiet_payloads = 1;
    } else if (argc == 3 && !strcmp(argv[1], "--block")) {
        char *end;
        block = strtol(argv[2], &end, 0);
        if (!*argv[2] || *end || block < 0 || block > 65535) return 2;
    } else if (argc == 3 && !strcmp(argv[1], "--mode") &&
               (!strcmp(argv[2], "1") || !strcmp(argv[2], "2"))) {
        mode = argv[2][0] - '0';
    } else if (argc == 2 && !strcmp(argv[1], "--info")) info_only = 1;
    else if (argc != 1) {
        fputs("Usage: probe [--info | --block ID | --mode 1|2 | --write-pin-block EXPECTED_FILE UPDATED_FILE]\n", stderr); return 2;
    }
    setbuf(stdout, NULL);
    IOHIDManagerRef manager = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    match_number(match, CFSTR(kIOHIDVendorIDKey), 0x0a12);
    match_number(match, CFSTR(kIOHIDProductIDKey), 0x4007);
    match_number(match, CFSTR(kIOHIDPrimaryUsagePageKey), 0xff00);
    match_number(match, CFSTR(kIOHIDPrimaryUsageKey), 1);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    CFIndex count = devices ? CFSetGetCount(devices) : 0;
    printf("Matching vendor HID interfaces: %ld\n", count);
    if (count != 1) { if (devices) CFRelease(devices); CFRelease(manager); return 2; }
    const void *item = NULL;
    CFSetGetValues(devices, &item);
    IOHIDDeviceRef device = (IOHIDDeviceRef)item;
    uint64_t registry_id = 0;
    if (IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(device), &registry_id) != KERN_SUCCESS)
        return 2;
    printf("REGISTRY_ID=%llu\n", (unsigned long long)registry_id);
    CFStringRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    if (!product || CFGetTypeID(product) != CFStringGetTypeID() ||
        !CFEqual(product, CFSTR("blafili B3"))) { puts("Unexpected product; stopped."); return 2; }
    IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    printf("Open blafili B3 (nonexclusive): 0x%08x (%s)\n", r, mach_error_string(r));
    if (r != kIOReturnSuccess) { CFRelease(devices); CFRelease(manager); return 3; }
    uint8_t input[66] = {0};
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(device, input, sizeof(input), input_report, NULL);
    int ok = query(device, 0x17, NULL, 0);
    if (ok) {
        int protocol_ok = 0, signature_ok = 0;
        if (query(device, 1, NULL, 0) && response_length == 6) {
            protocol_ok = be16(response + 2) == 0 && be16(response + 4) == 4;
            printf("AHI protocol version: %u.%u\n", be16(response + 2), be16(response + 4));
        }
        unsigned current_mode = 0;
        if (query(device, 9, NULL, 0) && response_length == 4) {
            current_mode = be16(response + 2);
            printf("MODE=%u\n", current_mode);
        }
        if (query(device, 0x1d, NULL, 0) && response_length == 6)
            printf("AHI product ID: 0x%08x\n", be32(response + 2));
        if (query(device, 0x1f, NULL, 0) && response_length == 4)
            printf("Application build ID: %u\n", be16(response + 2));
        if (query(device, 0x13, NULL, 0) && response_length >= 4) {
            signature_ok = response_length == 36 && be16(response + 2) == 1 &&
                !memcmp(response + 4, "5819ff8235a6d18029d414ec778a7f5c", 32);
            printf("Configuration signature type=%u value=", be16(response + 2));
            for (size_t i = 4; i < response_length; i++) putchar(response[i]);
            putchar('\n');
        }
        int changing_mode = 0;
        if (write_pin) {
            ok = protocol_ok && current_mode == 2 && signature_ok && !malformed;
            if (!ok) puts("PIN write refused: requires AHI 0.4, configuration mode 2, and the known B3 schema.");
            if (ok) {
                ok = read_block(device, 133) && block_bytes == sizeof(pin_before) &&
                    !memcmp(block_data, pin_before, sizeof(pin_before));
                if (!ok) puts("PIN write refused: live block does not match the complete expected baseline.");
            }
            if (ok && !memcmp(pin_before, pin_after, sizeof(pin_before))) {
                puts("PIN_WRITE=UNCHANGED VERIFIED=1");
            } else if (ok) {
                pin_write_authorized = 1;
                int acknowledged = query(device, 7, NULL, 0);
                printf("PIN_WRITE_ACKNOWLEDGED=%d\n", acknowledged);
                if (acknowledged) {
                    ok = read_block(device, 133) && block_bytes == sizeof(pin_after) &&
                        !memcmp(block_data, pin_after, sizeof(pin_after));
                    printf("PIN_WRITE_VERIFIED=%d\n", ok);
                } else {
                    ok = 0;
                    puts("PIN write outcome uncertain or rejected; no retry performed.");
                }
            }
        } else if (mode >= 0) {
            if (current_mode != 1 && current_mode != 2) {
                puts("Unknown current mode; stopped."); ok = 0;
            } else if (current_mode == (unsigned)mode) puts("Already in requested mode.");
            else {
                uint8_t value[2] = {0, (uint8_t)mode};
                ok = query(device, 0x0b, value, sizeof(value));
                ok = ok && response_length == 4 && be16(response + 2) == (unsigned)mode;
                changing_mode = ok && response_status == 1;
                printf("MODE_SET=%ld CONFIRMED=%d REBOOT_REQUIRED=%d\n", mode, ok, changing_mode);
            }
        } else if (block >= 0) {
            ok = read_block(device, (unsigned)block);
            if (ok) {
                char path[64];
                snprintf(path, sizeof(path), "block-%ld.bin", block);
                FILE *output = fopen(path, "wb");
                if (!output) { perror(path); ok = 0; }
                else {
                    ok = fwrite(block_data, 1, block_bytes, output) == block_bytes;
                    if (fclose(output) != 0) ok = 0;
                    if (ok) printf("Saved %s\n", path);
                }
            }
        } else if (!info_only) {
            schema = fopen("config-definition.bin", "wb");
            if (!schema) { perror("config-definition.bin"); ok = 0; }
            else {
                ok = query(device, 0x0d, NULL, 0);
                CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 30;
                while (ok && !schema_done && !malformed && CFAbsoluteTimeGetCurrent() < deadline)
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
                if (fclose(schema) != 0) ok = 0;
                schema = NULL;
                printf("Definition bytes: %zu / %zu; final packet: %s\n",
                       schema_bytes, schema_expected, schema_done ? "yes" : "no");
                ok = ok && !malformed && schema_done && schema_bytes == schema_expected;
            }
        }
        int disconnected = query(device, 0x19, NULL, 0);
        printf("Session closed: %s\n", disconnected ? "confirmed" : "NOT CONFIRMED");
        ok = ok && (disconnected || changing_mode);
    }
    IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
    CFRelease(devices); CFRelease(manager);
    return ok ? 0 : 4;
}
