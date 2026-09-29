/* Bounded Qualcomm 0a12:4010 parent-hub challenge/response probe.
 *
 * Build: clang -std=c11 -Wall -Wextra -Werror debug_unlock.c \
 *             -framework IOKit -framework CoreFoundation -o debug_unlock
 *
 * USBHubFilter.sys 112.2.0.0, SHA256:
 * bedcec0588d10d8e6b2d4dea8d2154cdf9712d67ba08c9876d99ac1718c5a3f0
 * Its URB builder at VA 0x14000184c sets request/value/index to zero.
 * Its only callers (0x140001c1a, 0x140001d31) transfer 16 bytes, first IN,
 * then OUT, using URB_FUNCTION_VENDOR_DEVICE. No additional USB request
 * or port feature is sent by this program. Successful OUT transport is
 * not proof of authentication acceptance or debug-child enumeration.
 */
#include <CommonCrypto/CommonCryptor.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { BLOCK_SIZE = 16, TIMEOUT_MS = 2000 };
typedef IOReturn (*request_fn)(void *, IOUSBDevRequestTO *);
struct exchange_result {
    IOReturn status;
    UInt32 challenge_length;
    UInt32 response_length;
    bool response_sent;
};

static bool encrypt_response(const uint8_t challenge[BLOCK_SIZE],
                             uint8_t response[BLOCK_SIZE]) {
    /* Driver crypto VA 0x140003e14: AES-128, raw key/challenge bytes,
     * CBC with IV at device context +0x48, initially zero. WDF zeroes
     * context allocations; there is no driver write to that IV before
     * encryption. BCryptEncrypt uses block padding and emits 32 bytes,
     * but the USB OUT sends only its first 16 bytes. That first block is
     * exactly AES-ECB(key, challenge), with no byte/word swapping.
     *
     * https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/framework-object-context-space
     * https://learn.microsoft.com/en-us/windows/win32/seccng/encrypting-data-with-cng
     * The Microsoft example uses the same IV for size query and real
     * encryption, resetting it only after encryption. The size query
     * does not consume the IV. This program performs one fresh exchange.
     *
     * The zero key is the documented unsecured-device key (Qualcomm
     * 80-CH362-1 Rev AG, sections 4.7/4.8); no custom or guessed key is tried.
     */
    const uint8_t key[BLOCK_SIZE] = {0};
    size_t written = 0;
    CCCryptorStatus status = CCCrypt(kCCEncrypt, kCCAlgorithmAES128,
        kCCOptionECBMode, key, sizeof(key), NULL, challenge, BLOCK_SIZE,
        response, BLOCK_SIZE, &written);
    return status == kCCSuccess && written == BLOCK_SIZE;
}

static IOUSBDevRequestTO vendor_request(bool output, void *buffer) {
    IOUSBDevRequestTO request = {0};
    request.bmRequestType = output ? 0x40 : 0xc0;
    request.wLength = BLOCK_SIZE;
    request.pData = buffer;
    request.noDataTimeout = TIMEOUT_MS;
    request.completionTimeout = TIMEOUT_MS;
    return request;
}

static struct exchange_result exchange(void *context, request_fn request,
                                       bool unlock) {
    struct exchange_result result = {0};
    uint8_t challenge[BLOCK_SIZE] = {0}, response[BLOCK_SIZE] = {0};
    IOUSBDevRequestTO input = vendor_request(false, challenge);
    result.status = request(context, &input);
    result.challenge_length = input.wLenDone;
    if (result.status != kIOReturnSuccess) return result;
    if (input.wLenDone != BLOCK_SIZE) {
        result.status = kIOReturnUnderrun;
        return result;
    }
    if (!unlock) return result;
    if (!encrypt_response(challenge, response)) {
        result.status = kIOReturnInternalError;
        return result;
    }
    IOUSBDevRequestTO output = vendor_request(true, response);
    result.response_sent = true;
    result.status = request(context, &output);
    result.response_length = output.wLenDone;
    if (result.status == kIOReturnSuccess && output.wLenDone != BLOCK_SIZE)
        result.status = kIOReturnUnderrun;
    return result;
}

struct fake_transport {
    unsigned calls;
    UInt32 in_length;
    IOReturn in_status;
    IOReturn out_status;
    bool valid;
};

static const uint8_t zero_block_cipher[BLOCK_SIZE] = {
    0x66, 0xe9, 0x4b, 0xd4, 0xef, 0x8a, 0x2c, 0x3b,
    0x88, 0x4c, 0xfa, 0x59, 0xca, 0x34, 0x2b, 0x2e
};

static IOReturn fake_request(void *context, IOUSBDevRequestTO *request) {
    struct fake_transport *fake = context;
    unsigned index = fake->calls++;
    fake->valid = fake->valid && index < 2 &&
        request->bmRequestType == (index == 0 ? 0xc0 : 0x40) &&
        request->bRequest == 0 && request->wValue == 0 &&
        request->wIndex == 0 && request->wLength == BLOCK_SIZE &&
        request->wLenDone == 0 && request->pData != NULL &&
        request->noDataTimeout == TIMEOUT_MS &&
        request->completionTimeout == TIMEOUT_MS;
    if (index == 0) {
        memset(request->pData, 0, BLOCK_SIZE);
        request->wLenDone = fake->in_length;
        return fake->in_status;
    }
    fake->valid = fake->valid &&
        memcmp(request->pData, zero_block_cipher, BLOCK_SIZE) == 0;
    request->wLenDone = BLOCK_SIZE;
    return fake->out_status;
}

static int self_test(void) {
    uint8_t challenge[BLOCK_SIZE] = {0};
    uint8_t response[BLOCK_SIZE] = {0};
    if (!encrypt_response(challenge, response) ||
        memcmp(response, zero_block_cipher, BLOCK_SIZE) != 0) {
        fputs("Self-test failed: AES-128 zero key/block vector.\n", stderr);
        return 1;
    }
    for (unsigned test = 0; test < 5; test++) {
        struct fake_transport fake = {
            .in_length = test == 2 ? 15 : BLOCK_SIZE,
            .in_status = test == 3 ? kIOReturnTimeout : kIOReturnSuccess,
            .out_status = test == 4 ? kIOReturnTimeout : kIOReturnSuccess,
            .valid = true
        };
        bool unlock = test != 0;
        struct exchange_result result = exchange(&fake, fake_request, unlock);
        bool should_send = test == 1 || test == 4;
        bool should_succeed = test < 2;
        if (!fake.valid || fake.calls != (should_send ? 2u : 1u) ||
            result.response_sent != should_send ||
            (result.status == kIOReturnSuccess) != should_succeed) {
            fprintf(stderr, "Self-test failed: bounded exchange case %u.\n", test);
            return 1;
        }
    }
    puts("Self-test passed: AES vector, exact packets, short read, timeouts, no retries.");
    return 0;
}

static bool add_number(CFMutableDictionaryRef match, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &value);
    if (!number) return false;
    CFDictionarySetValue(match, key, number);
    CFRelease(number);
    return true;
}

static bool property_is(io_registry_entry_t service, CFStringRef key, int expected) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
    int actual = 0;
    bool valid = value && CFGetTypeID(value) == CFNumberGetTypeID() &&
        CFNumberGetValue(value, kCFNumberIntType, &actual) && actual == expected;
    if (value) CFRelease(value);
    return valid;
}

static IOUSBDeviceInterface650 **find_hub(void) {
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (!match) return NULL;
    if (!add_number(match, CFSTR("idVendor"), 0x0a12) ||
        !add_number(match, CFSTR("idProduct"), 0x4007)) {
        CFRelease(match);
        return NULL;
    }
    io_iterator_t iterator = IO_OBJECT_NULL;
    IOReturn status = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);
    if (status != kIOReturnSuccess) {
        fprintf(stderr, "USB enumeration failed: 0x%08x\n", status);
        return NULL;
    }
    io_service_t child = IOIteratorNext(iterator), extra = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!child || extra) {
        fputs("Expected exactly one B3 0a12:4007 USB device.\n", stderr);
        if (child) IOObjectRelease(child);
        if (extra) IOObjectRelease(extra);
        return NULL;
    }
    io_registry_entry_t hub = IO_OBJECT_NULL;
    status = IORegistryEntryGetParentEntry(child, "IOUSB", &hub);
    IOObjectRelease(child);
    if (status != kIOReturnSuccess || !hub) {
        fputs("Cannot identify the B3 immediate USB parent.\n", stderr);
        return NULL;
    }
    bool valid = property_is(hub, CFSTR("idVendor"), 0x0a12) &&
        property_is(hub, CFSTR("idProduct"), 0x4010) &&
        property_is(hub, CFSTR("bDeviceClass"), 9) &&
        property_is(hub, CFSTR("bDeviceProtocol"), 0);
    if (!valid) {
        fputs("B3 parent does not match Qualcomm 0a12:4010 class 9/protocol 0.\n", stderr);
        IOObjectRelease(hub);
        return NULL;
    }
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    status = IOCreatePlugInInterfaceForService(hub, kIOUSBDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(hub);
    if (status != kIOReturnSuccess || !plugin) {
        fprintf(stderr, "Hub USB plugin unavailable: 0x%08x\n", status);
        if (plugin) (*plugin)->Release(plugin);
        return NULL;
    }
    IOUSBDeviceInterface650 **device = NULL;
    HRESULT query = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&device);
    (*plugin)->Release(plugin);
    if (query || !device) {
        fputs("Hub USB device interface unavailable.\n", stderr);
        if (device) (*device)->Release(device);
        return NULL;
    }
    return device;
}

static IOReturn native_request(void *context, IOUSBDevRequestTO *request) {
    IOUSBDeviceInterface650 **device = context;
    /* DeviceRequestTO permits vendor requests without USBDeviceOpen.
     * Do not open or seize this hub, which has an active OS driver. */
    return (*device)->DeviceRequestTO(device, request);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return self_test();
    bool challenge_only = argc == 2 && strcmp(argv[1], "--challenge") == 0;
    bool unlock = argc == 2 && strcmp(argv[1], "--unlock-default") == 0;
    if (!challenge_only && !unlock) {
        fputs("Usage: debug_unlock --self-test | --challenge | --unlock-default\n", stderr);
        return 2;
    }
    IOUSBDeviceInterface650 **device = find_hub();
    if (!device) return 1;
    struct exchange_result result = exchange(device, native_request, unlock);
    (*device)->Release(device);
    if (result.status != kIOReturnSuccess) {
        fprintf(stderr, "Exchange failed: 0x%08x; challenge bytes=%u, "
            "response attempted=%s, response bytes=%u. No retries.\n",
            result.status, (unsigned)result.challenge_length,
            result.response_sent ? "yes" : "no", (unsigned)result.response_length);
        return 1;
    }
    printf("Challenge received: %u bytes.\n", (unsigned)result.challenge_length);
    if (unlock)
        puts("Default-key response transfer completed once. Debug enumeration remains unverified.");
    return 0;
}
