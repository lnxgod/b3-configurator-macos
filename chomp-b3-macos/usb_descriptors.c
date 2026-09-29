/* Read cached USB configuration descriptors for the connected B3.
 * --listen additionally selects USB configuration 1 if the debug device is
 * unconfigured, opens its interface without seizing it, and performs one
 * bounded bulk IN. Fixed queries also support service, chip and configuration
 * reads. --reboot sends the documented curator reboot request once. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "name_transport.h"

static const char *read_image_path = NULL;
static unsigned char name_before[2044], name_after[2044];
static size_t name_before_length = 0, name_after_length = 0;
static int write_name_requested = 0;

static int load_image(const char *path, unsigned char *data, size_t *length) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 2044) {
        close(fd);
        return 0;
    }
    *length = (size_t)st.st_size;
    size_t done = 0;
    while (done < *length) {
        ssize_t n = read(fd, data + done, *length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return 0; }
        done += (size_t)n;
    }
    unsigned char extra;
    ssize_t trailing = read(fd, &extra, 1);
    close(fd);
    return trailing == 0;
}

struct name_usb_context {
    IOUSBInterfaceInterface550 **interface;
    UInt8 input, output;
};

static int name_usb_exchange(void *context, const unsigned char *tx, size_t tx_length,
                             unsigned char *rx, size_t *rx_length, unsigned timeout_ms) {
    struct name_usb_context *usb = context;
    if (tx_length > 4095 || *rx_length > 4096) return -1;
    IOReturn status = (*usb->interface)->WritePipeTO(usb->interface, usb->output,
        (void *)tx, (UInt32)tx_length, timeout_ms, timeout_ms);
    /* USBDBG messages terminate on short packets, including a ZLP when the
     * payload occupies an exact number of 64-byte USB packets. */
    if (!status && tx_length % 64 == 0) {
        UInt8 empty = 0;
        status = (*usb->interface)->WritePipeTO(usb->interface, usb->output,
            &empty, 0, timeout_ms, timeout_ms);
    }
    UInt32 length = (UInt32)*rx_length;
    if (!status) status = (*usb->interface)->ReadPipeTO(usb->interface, usb->input,
        rx, &length, timeout_ms, timeout_ms);
    *rx_length = status ? 0 : length;
    return status ? -1 : 0;
}

static unsigned le16(const UInt8 *p) { return p[0] | (unsigned)p[1] << 8; }

static IOReturn read_config(IOUSBInterfaceInterface550 **interface,
                          UInt8 input, UInt8 output) {
    /* Create privately before sending anything; never overwrite an existing backup. */
    int fd = open(read_image_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { perror("Configuration output"); return kIOReturnError; }
    UInt8 request[] = {3,0,0x24,0x2a,9,0,3,0,3,0,1,0,0x34,0x12,0,0,4,0xfa,0xfa,0xfa};
    UInt8 response[4096] = {0};
    UInt32 length = sizeof(response);
    IOReturn status = (*interface)->WritePipeTO(interface, output, request, sizeof(request), 2000, 2000);
    if (!status) status = (*interface)->ReadPipeTO(interface, input, response, &length, 5000, 5000);
    printf("CONFIG_READ status=0x%08x frame_bytes=%u\n", status, status ? 0 : (unsigned)length);
    const UInt8 header[] = {3,0,0x91,0x28,9,0};
    size_t file_length = 0;
    if (!status) {
        if (length < 17 || length >= sizeof(response) || memcmp(response, header, sizeof(header))) status = kIOReturnBadArgument;
        else {
            unsigned padding = le16(response + 6) & 3;
            unsigned pdu_length = length - 8 - padding;
            if (pdu_length < 9 || le16(response + 8) != 3 ||
                le16(response + 10) != pdu_length - 8 || le16(response + 14) != 0 ||
                response[16] != 4 || (pdu_length - 9) % 2) status = kIOReturnBadArgument;
            else file_length = pdu_length - 9;
        }
    }
    if (!status) {
        size_t written = 0;
        while (written < file_length) {
            ssize_t n = write(fd, response + 17 + written, file_length - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { status = kIOReturnError; break; }
            written += (size_t)n;
        }
        if (!status && fsync(fd)) status = kIOReturnError;
    }
    if (close(fd) && !status) status = kIOReturnError;
    if (!status) printf("CONFIG_IMAGE bytes=%zu saved privately\n", file_length);
    else fprintf(stderr, "No validated configuration image saved (output may be empty/partial).\n");
    return status;
}

static void number(CFMutableDictionaryRef match, CFStringRef key, int value) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);
    CFDictionarySetValue(match, key, n);
    CFRelease(n);
}

/* A TestTunnel connection allocates a temporary PTCMD session, performs the
 * requested guarded operation, then closes it. Raw responses are never logged:
 * after a timeout a delayed configuration frame could reach the cleanup read. */
static IOReturn tunnel_session(IOUSBInterfaceInterface550 **interface,
                              UInt8 input, UInt8 output) {
    UInt8 connect[] = {3,0,0x24,0x2a,0,0,0,0,0,0,9,0};
    UInt8 disconnect[] = {3,0,0x24,0x2a,0,0,0,0,2,0,9,0};
    UInt8 response[4096] = {0};
    UInt32 length = sizeof(response);
    IOReturn status = (*interface)->WritePipeTO(interface, output, connect, sizeof(connect), 2000, 2000);
    if (!status) status = (*interface)->ReadPipeTO(interface, input, response, &length, 2000, 2000);
    printf("TUNNEL_CONNECT status=0x%08x bytes=%u\n", status, status ? 0 : (unsigned)length);
    const UInt8 expected[] = {3,0,0x91,0x28,0,0,0,0,1,0,9,0,0,0};
    if (!status && (length != 16 || memcmp(response, expected, sizeof(expected)))) status = kIOReturnBadArgument;
    IOReturn operation_status = status;
    if (!operation_status && read_image_path) operation_status = read_config(interface, input, output);
    if (!operation_status && write_name_requested) {
        struct name_usb_context usb = {interface, input, output};
        int attempted = 0;
        int result = b3_name_write(name_usb_exchange, &usb,
            name_before, name_before_length, name_after, name_after_length, &attempted);
        printf("NAME_WRITE status=%d attempted=%d\n", result, attempted);
        if (result == 0) puts("NAME_WRITE_VERIFIED=1");
        else operation_status = kIOReturnError;
    }
    status = (*interface)->WritePipeTO(interface, output, disconnect, sizeof(disconnect), 2000, 2000);
    length = sizeof(response);
    if (!status) status = (*interface)->ReadPipeTO(interface, input, response, &length, 2000, 2000);
    printf("TUNNEL_DISCONNECT status=0x%08x bytes=%u\n", status, status ? 0 : (unsigned)length);
    if (!status) {
        const UInt8 closed[] = {3,0,0x91,0x28,0,0,0,0,3,0,9,0,0,0};
        if (length != 16 || memcmp(response, closed, sizeof(closed))) status = kIOReturnBadArgument;
    }
    return operation_status ? operation_status : status;
}

static IOReturn listen_once(IOUSBDeviceInterface650 **device, int query_mode) {
    UInt8 configuration = 0;
    IOReturn status = (*device)->GetConfiguration(device, &configuration);
    if (status) return status;
    if (configuration != 0 && configuration != 1) return kIOReturnBadArgument;
    if (configuration == 0) {
        status = (*device)->USBDeviceOpen(device);
        if (status) return status;
        status = (*device)->SetConfiguration(device, 1);
        (*device)->USBDeviceClose(device);
        if (status) return status;
        puts("Selected debug USB configuration 1.");
    }
    IOUSBFindInterfaceRequest request = {0xff, 0, 0, 0};
    io_iterator_t iterator = IO_OBJECT_NULL;
    status = (*device)->CreateInterfaceIterator(device, &request, &iterator);
    if (status) return status;
    io_service_t service = IOIteratorNext(iterator), extra = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!service || extra) {
        if (service) IOObjectRelease(service);
        if (extra) IOObjectRelease(extra);
        return kIOReturnNotFound;
    }
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    status = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (status || !plugin) return status ? status : kIOReturnNotFound;
    IOUSBInterfaceInterface550 **interface = NULL;
    HRESULT query = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID550), (LPVOID *)&interface);
    (*plugin)->Release(plugin);
    if (query || !interface) return kIOReturnUnsupported;
    status = (*interface)->USBInterfaceOpen(interface);
    if (!status) {
        UInt8 count = 0, input = 0, output = 0;
        status = (*interface)->GetNumEndpoints(interface, &count);
        for (UInt8 pipe = 1; !status && pipe <= count; pipe++) {
            UInt8 direction = 0, endpoint = 0, type = 0, interval = 0;
            UInt16 max_packet = 0;
            status = (*interface)->GetPipeProperties(interface, pipe, &direction,
                &endpoint, &type, &max_packet, &interval);
            if (!status && direction == kUSBIn && endpoint == 1 &&
                type == kUSBBulk && max_packet == 64) input = pipe;
            if (!status && direction == kUSBOut && endpoint == 1 &&
                type == kUSBBulk && max_packet == 64) output = pipe;
        }
        if (!status && (!input || (query_mode && !output))) status = kIOReturnNotFound;
        if (!status && query_mode == 3) {
            status = tunnel_session(interface, input, output);
            (*interface)->USBInterfaceClose(interface);
            (*interface)->Release(interface);
            return status;
        }
        if (!status && query_mode == 4) {
            /* ToolCmdSession::RebootCurator(0), opcode 0x0030. Vendor client
             * deliberately expects no reply because the transport disappears. */
            UInt8 reboot[] = {3,0,0x20,0x22,0x30,0,0,0};
            status = (*interface)->WritePipeTO(interface, output, reboot, sizeof(reboot), 2000, 2000);
            printf("REBOOT_TX status=0x%08x (no reply expected; verify re-enumeration)\n", status);
            (*interface)->USBInterfaceClose(interface);
            (*interface)->Release(interface);
            return status;
        }
        if (!status && query_mode) {
            /* Hydra.sys hydra_service_advertisement_query_req, little endian.
             * Builder VA 0x140008bb8; normal USB send 0x14000d348 prepends
             * 03 00, ISP header 00 02 = source client 0x10, destination 0.
             * Fixed read-only service discovery request; no arbitrary TX CLI. */
            UInt8 packet[] = {0x03,0x00,0x00,0x02,0x17,0x00,0x00,0x00,0x00,0x00,0xff,0x7f,0x00,0x01};
            /* ToolCmd read_windowed: curator 0, word address 0xfe81,
             * one word. ISP host address 0x11 -> curator port 8. */
            UInt8 chip[] = {0x03,0x00,0x20,0x22,0x02,0x00,0x81,0xfe,0x00,0x00,0x10,0x00};
            status = (*interface)->WritePipeTO(interface, output,
                query_mode == 2 ? chip : packet,
                query_mode == 2 ? sizeof(chip) : sizeof(packet), 2000, 2000);
            printf("QUERY_TX status=0x%08x\n", status);
        }
        if (!status) {
            UInt8 bytes[4096] = {0};
            UInt32 length = sizeof(bytes);
            status = (*interface)->ReadPipeTO(interface, input, bytes, &length, 2000, 2000);
            printf("%s status=0x%08x bytes=%u\n", query_mode ? "QUERY_READ" : "PASSIVE_READ",
                   status, status ? 0 : (unsigned)length);
            if (!status) {
                for (UInt32 i = 0; i < length; i++) printf("%02x%s", bytes[i], (i+1)%16 ? " " : "\n");
                if (length%16) putchar('\n');
            }
        }
        (*interface)->USBInterfaceClose(interface);
    }
    (*interface)->Release(interface);
    return status;
}

int main(int argc, char **argv) {
    int hub = argc == 2 && strcmp(argv[1], "--hub") == 0;
    int listen = argc == 2 && strcmp(argv[1], "--listen") == 0;
    int service_query = argc == 2 && strcmp(argv[1], "--service-query") == 0;
    int chip_id = argc == 2 && strcmp(argv[1], "--chip-id") == 0;
    int tunnel = argc == 2 && strcmp(argv[1], "--tunnel-session") == 0;
    int reboot = argc == 2 && strcmp(argv[1], "--reboot") == 0;
    if (argc == 3 && strcmp(argv[1], "--read-config") == 0) {
        read_image_path = argv[2];
        tunnel = 1;
    }
    if (argc == 4 && strcmp(argv[1], "--write-name-image") == 0) {
        if (!load_image(argv[2], name_before, &name_before_length) ||
            !load_image(argv[3], name_after, &name_after_length) ||
            !b3_name_only_change(name_before, name_before_length, name_after, name_after_length)) {
            fputs("Rejected: expected and updated images must differ only in the Bluetooth name.\n", stderr);
            return 2;
        }
        tunnel = 1;
        write_name_requested = 1;
    }
    int debug = listen || service_query || chip_id || tunnel || reboot || (argc == 2 && strcmp(argv[1], "--debug") == 0);
    if (argc != 1 && !hub && !debug) {
        fputs("Usage: usb_descriptors [--hub|--debug|--listen|--service-query|--chip-id|--tunnel-session|--read-config NEW_FILE|--reboot]\n", stderr);
        return 2;
    }
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    number(match, CFSTR("idVendor"), 0x0a12);
    number(match, CFSTR("idProduct"), 0x4007);
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator)) return 1;
    io_service_t service = IOIteratorNext(iterator), extra = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!service || extra) {
        fputs("Expected exactly one B3 USB device.\n", stderr);
        if (service) IOObjectRelease(service);
        if (extra) IOObjectRelease(extra);
        return 1;
    }
    if (hub || debug) {
        io_registry_entry_t parent = IO_OBJECT_NULL;
        IOReturn parent_status = IORegistryEntryGetParentEntry(service, "IOUSB", &parent);
        IOObjectRelease(service);
        if (parent_status || !parent) return 1;
        CFTypeRef vid = IORegistryEntryCreateCFProperty(parent, CFSTR("idVendor"), NULL, 0);
        CFTypeRef pid = IORegistryEntryCreateCFProperty(parent, CFSTR("idProduct"), NULL, 0);
        int vendor = 0, product = 0;
        int valid = vid && pid && CFGetTypeID(vid) == CFNumberGetTypeID() &&
                    CFGetTypeID(pid) == CFNumberGetTypeID() &&
                    CFNumberGetValue(vid, kCFNumberIntType, &vendor) &&
                    CFNumberGetValue(pid, kCFNumberIntType, &product) &&
                    vendor == 0x0a12 && product == 0x4010;
        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        if (!valid) {
            fputs("B3 parent is not the expected Qualcomm 0a12:4010 hub.\n", stderr);
            IOObjectRelease(parent);
            return 1;
        }
        service = parent;
        if (debug) {
            io_iterator_t children = IO_OBJECT_NULL;
            IOReturn child_status = IORegistryEntryGetChildIterator(parent, "IOUSB", &children);
            io_service_t found = IO_OBJECT_NULL, child;
            int count = 0, invalid_child = 0;
            if (!child_status) while ((child = IOIteratorNext(children))) {
                CFTypeRef child_pid = IORegistryEntryCreateCFProperty(child, CFSTR("idProduct"), NULL, 0);
                CFTypeRef child_vid = IORegistryEntryCreateCFProperty(child, CFSTR("idVendor"), NULL, 0);
                int p = 0, v = 0;
                int readable = child_pid && child_vid && CFGetTypeID(child_pid) == CFNumberGetTypeID() &&
                    CFGetTypeID(child_vid) == CFNumberGetTypeID() &&
                    CFNumberGetValue(child_pid, kCFNumberIntType, &p) &&
                    CFNumberGetValue(child_vid, kCFNumberIntType, &v);
                if (!readable) invalid_child = 1;
                if (readable && p == 0x4000 && v == 0x0a12) {
                    count++;
                    if (!found) { found = child; IOObjectRetain(found); }
                }
                if (child_pid) CFRelease(child_pid);
                if (child_vid) CFRelease(child_vid);
                IOObjectRelease(child);
            }
            if (children && !IOIteratorIsValid(children)) invalid_child = 1;
            if (children) IOObjectRelease(children);
            IOObjectRelease(parent);
            if (child_status || invalid_child || count != 1) {
                if (found) IOObjectRelease(found);
                if (!child_status && !invalid_child && count == 0) puts("DEBUG_PRESENT=0");
                fputs("Expected one 0a12:4000 sibling of the B3.\n", stderr);
                return 1;
            }
            service = found;
        }
    }
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn status = IOCreatePlugInInterfaceForService(service,
        kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (status || !plugin) {
        fprintf(stderr, "USB plugin unavailable: 0x%08x\n", status);
        return 1;
    }
    IOUSBDeviceInterface650 **device = NULL;
    HRESULT query = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&device);
    (*plugin)->Release(plugin);
    if (query || !device) return 1;
    UInt8 count = 0;
    status = (*device)->GetNumberOfConfigurations(device, &count);
    for (UInt8 index = 0; !status && index < count; index++) {
        IOUSBConfigurationDescriptorPtr config = NULL;
        status = (*device)->GetConfigurationDescriptorPtr(device, index, &config);
        if (status || !config) break;
        const UInt8 *bytes = (const UInt8 *)config;
        unsigned total = bytes[2] | (unsigned)bytes[3] << 8;
        printf("CONFIG index=%u value=%u interfaces=%u bytes=%u\n",
               index, config->bConfigurationValue, config->bNumInterfaces, total);
        for (unsigned offset = 0; offset + 2 <= total;) {
            const UInt8 *d = bytes + offset;
            if (d[0] < 2 || offset + d[0] > total) { status = kIOReturnBadArgument; break; }
            if (d[1] == 4 && d[0] >= 9)
                printf("  INTERFACE number=%u alt=%u endpoints=%u class=%02x/%02x/%02x\n",
                       d[2], d[3], d[4], d[5], d[6], d[7]);
            if (d[1] == 5 && d[0] >= 7)
                printf("    ENDPOINT address=%02x attributes=%02x max_packet=%u interval=%u\n",
                       d[2], d[3], d[4] | (unsigned)d[5] << 8, d[6]);
            offset += d[0];
        }
    }
    if (!status && (listen || service_query || chip_id || tunnel || reboot)) status = listen_once(device, reboot ? 4 : tunnel ? 3 : chip_id ? 2 : service_query);
    (*device)->Release(device);
    if (status) fprintf(stderr, "USB operation failed: 0x%08x\n", status);
    return status ? 1 : 0;
}
