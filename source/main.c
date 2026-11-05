/*
 * USB Hub Diagnostics - Wii homebrew (libogc, IOS58 / /dev/usb/ven)
 *
 * READ-ONLY. Never creates, writes, deletes, formats or mounts read/write.
 * Nothing is written to NAND, SD or USB. Results are shown on screen only.
 *
 *  1. USB device + interface/endpoint enumeration (libogc usb.h)
 *  2. Mass-storage LUN probe (libogc usbstorage.h)
 *  3. Read-only FAT mount (libfat) + capacity + directory listing
 *  4. Read-only benchmarks (raw sectors = USB path, files = FS+cache path)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <gccore.h>
#include <fat.h>
#include <ogc/usb.h>
#include <ogc/usbstorage.h>
#include <ogc/disc_io.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#define MAX_DEV   32
#define LOG_MAX   (96 * 1024)
#define LOG_NAME  "usb_diag_log"
#define CHUNK_SEC 128           /* 64 KiB at 512 B/sector */
#define MAX_FILES 400

static char logbuf[LOG_MAX];
static u32 loglen;
static int lp(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    if (fmt[0] != 0x1b && loglen < LOG_MAX - 512) {
        va_start(ap, fmt);
        int n = vsnprintf(logbuf + loglen, 512, fmt, ap);
        va_end(ap);
        if (n > 0) loglen += (n > 511) ? 511 : n;
    }
    return r;
}
#define printf lp

static GXRModeObj *rmode;
static void *xfb;

/* ------------------------------------------------------------ UI helpers */
static void video_init(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth - 40, rmode->xfbHeight - 40,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

static void quit(void) { SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0); }

#define BTN_A    1
#define BTN_B    2
#define BTN_HOME 4
/* Wiimote 1 (A/B/HOME) or any GameCube pad port 1-4 (A/B/START) */
static int poll_btn(void)
{
    WPAD_ScanPads();
    PAD_ScanPads();
    u32 w = WPAD_ButtonsDown(0), g = 0;
    for (int i = 0; i < 4; i++) g |= PAD_ButtonsDown(i);
    int r = 0;
    if ((w & WPAD_BUTTON_A) || (g & PAD_BUTTON_A)) r |= BTN_A;
    if ((w & WPAD_BUTTON_B) || (g & PAD_BUTTON_B)) r |= BTN_B;
    if ((w & WPAD_BUTTON_HOME) || (g & PAD_BUTTON_START)) r |= BTN_HOME;
    return r;
}

static void wait_a(const char *msg)
{
    printf("\n-- %s (A=continue, HOME/START=exit) --\n", msg);
    for (;;) {
        int d = poll_btn();
        if (d & BTN_HOME) quit();
        if (d & BTN_A) break;
        VIDEO_WaitVSync();
    }
}

static void page(const char *title)
{
    printf("\x1b[2J\x1b[2;0H");
    printf("=== USB Hub Diagnostics: %s ===\n\n", title);
}

static u32 ms_since(u64 t0) { return (u32)ticks_to_millisecs(gettime() - t0); }

static void print_rate(const char *label, u64 bytes, u32 ms, u32 errs)
{
    double mbps = ms ? ((double)bytes / (1024.0 * 1024.0)) / ((double)ms / 1000.0) : 0.0;
    printf("%-14s %8.2f MiB  %6u ms  %6.2f MiB/s  err=%u\n",
           label, (double)bytes / (1024.0 * 1024.0), ms, mbps, errs);
}

static u32 adler(const u8 *p, u32 n, u32 a)
{
    u32 s1 = a & 0xffff, s2 = a >> 16;
    while (n--) { s1 = (s1 + *p++) % 65521; s2 = (s2 + s1) % 65521; }
    return (s2 << 16) | s1;
}

static const char *class_name(u8 c)
{
    switch (c) {
    case 0x00: return "per-interface"; case 0x01: return "Audio";
    case 0x02: return "CDC-comm";      case 0x03: return "HID";
    case 0x06: return "Image";         case 0x07: return "Printer";
    case 0x08: return "Mass Storage";  case 0x09: return "Hub";
    case 0x0A: return "CDC-data";      case 0x0E: return "Video";
    case 0xEF: return "Misc";          case 0xFE: return "App-specific";
    case 0xFF: return "Vendor-specific";
    default:   return "other";
    }
}

/* -------------------------------------------------- 1. USB enumeration */
static usb_device_entry devs[MAX_DEV];
static u8 ndevs;

static void ascii_string(s32 fd, u8 idx, char *out, u32 outlen)
{
    u8 *buf = memalign(32, 64);
    out[0] = 0;
    if (idx && buf) {
        memset(buf, 0, 64);
        if (USB_GetAsciiString(fd, idx, 0x0409, 62, buf) > 0) {
            strncpy(out, (char *)buf, outlen - 1);
            out[outlen - 1] = 0;
        }
    }
    free(buf);
}

static int mass_storage_count;

static void enumerate_usb(void)
{
    page("1/4 USB device enumeration");
    s32 r = USB_Initialize();
    printf("USB_Initialize: %d  (IOS %d)\n", (int)r, IOS_GetVersion());
    if (r < 0) { printf("ERROR: cannot init USB. IOS lacks /dev/usb/ven|oh0?\n"); return; }

    /* ven/hid backends fill their list asynchronously: give IOS time */
    for (int i = 0; i < 180; i++) VIDEO_WaitVSync();   /* ~3 s */

    ndevs = 0;
    memset(devs, 0, sizeof(devs));
    r = USB_GetDeviceList(devs, MAX_DEV, 0, &ndevs);
    printf("USB_GetDeviceList: %d, %u device(s)\n", (int)r, ndevs);
    if (r < 0) { printf("ERROR: device list failed.\n"); return; }

    for (u8 i = 0; i < ndevs; i++) {
        usb_devdesc dd;
        s32 fd = -1;
        printf("\n[%u] id=0x%x VID:PID=%04x:%04x\n", i, (unsigned)devs[i].device_id,
               devs[i].vid, devs[i].pid);
        r = USB_OpenDevice(devs[i].device_id, devs[i].vid, devs[i].pid, &fd);
        if (r < 0) { printf("   open failed: %d\n", (int)r); continue; }
        memset(&dd, 0, sizeof(dd));
        r = USB_GetDescriptors(fd, &dd);
        if (r < 0) { printf("   descriptors failed: %d\n", (int)r); USB_CloseDevice(&fd); continue; }

        char man[40], prod[40];
        ascii_string(fd, dd.iManufacturer, man, sizeof(man));
        ascii_string(fd, dd.iProduct, prod, sizeof(prod));
        printf("   USB %x.%02x class=%02x(%s) mfr='%s' prod='%s'\n",
               dd.bcdUSB >> 8, dd.bcdUSB & 0xff, dd.bDeviceClass,
               class_name(dd.bDeviceClass), man, prod);

        for (u8 c = 0; c < dd.bNumConfigurations; c++) {
            usb_configurationdesc *cd = &dd.configurations[c];
            printf("   cfg %u: %u interface(s), maxpower %u mA\n",
                   cd->bConfigurationValue, cd->bNumInterfaces, cd->bMaxPower * 2);
            for (u8 n = 0; n < cd->bNumInterfaces; n++) {
                usb_interfacedesc *id = &cd->interfaces[n];
                printf("    if%u alt%u class=%02x(%s) sub=%02x proto=%02x eps=%u\n",
                       id->bInterfaceNumber, id->bAlternateSetting,
                       id->bInterfaceClass, class_name(id->bInterfaceClass),
                       id->bInterfaceSubClass, id->bInterfaceProtocol, id->bNumEndpoints);
                if (id->bInterfaceClass == 0x08 && id->bInterfaceProtocol == 0x50)
                    mass_storage_count++;
                for (u8 e = 0; e < id->bNumEndpoints; e++) {
                    usb_endpointdesc *ed = &id->endpoints[e];
                    static const char *t[] = {"ctrl", "ISO", "bulk", "intr"};
                    printf("      ep %02x %s %s mps=%u\n", ed->bEndpointAddress,
                           (ed->bEndpointAddress & 0x80) ? "IN " : "OUT",
                           t[ed->bmAttributes & 3], ed->wMaxPacketSize);
                }
            }
        }
        USB_FreeDescriptors(&dd);
        USB_CloseDevice(&fd);
        if ((i % 2) == 1 && i + 1 < ndevs) wait_a("more devices");
    }
    printf("\nBulk-only mass-storage interfaces: %d\n", mass_storage_count);
}

/* ------------------------------------------- 2. LUN probe (own handle) */
static void probe_luns(void)
{
    page("2/4 Mass-storage LUN probe");
    USBStorage_Initialize();
    int found = 0;
    for (u8 i = 0; i < ndevs; i++) {
        usbstorage_handle h;
        s32 r = USBStorage_Open(&h, devs[i].device_id, devs[i].vid, devs[i].pid);
        if (r < 0) continue;     /* not mass storage (or open failed) */
        found++;
        s32 maxlun = USBStorage_GetMaxLUN(&h);
        printf("Dev %04x:%04x: %d LUN(s)\n", devs[i].vid, devs[i].pid, (int)maxlun);
        for (int l = 0; l < maxlun; l++) {
            s32 m = USBStorage_MountLUN(&h, l);
            u32 ss = 0, ns = 0;
            if (m >= 0 && USBStorage_ReadCapacity(&h, l, &ss, &ns) >= 0 && ss)
                printf("  LUN %d: media present, %u sectors x %u B = %.1f MiB\n",
                       l, ns, ss, ((double)ns * ss) / 1048576.0);
            else
                printf("  LUN %d: no media / not ready (code %d)\n", l, (int)m);
        }
        USBStorage_Close(&h);
    }
    if (!found) printf("No device opened as bulk-only mass storage.\n");
    printf("\nNOTE: libogc's built-in __io_usbstorage mounts only the FIRST LUN\n"
           "that answers. A 2-slot reader with both slots filled exposes just one.\n");
}

/* ----------------- 3. Read-only wrapper around __io_usbstorage + FAT */
static bool ro_startup(void)      { return __io_usbstorage.startup(); }
static bool ro_inserted(void)     { return __io_usbstorage.isInserted(); }
static bool ro_read(sec_t s, sec_t n, void *b) { return __io_usbstorage.readSectors(s, n, b); }
static bool ro_write(sec_t s, sec_t n, const void *b) { (void)s; (void)n; (void)b; return false; }
static bool ro_clear(void)        { return __io_usbstorage.clearStatus(); }
static bool ro_shutdown(void)     { return __io_usbstorage.shutdown(); }

/* No FEATURE_MEDIUM_CANWRITE => libfat sets partition->readOnly (partition.c) */
static const DISC_INTERFACE ro_usb = {
    DEVICE_TYPE_WII_USB, FEATURE_MEDIUM_CANREAD | FEATURE_WII_USB,
    ro_startup, ro_inserted, ro_read, ro_write, ro_clear, ro_shutdown
};

static u64 g_total_sectors;
static u32 g_sector_size = 512;
static bool g_mounted;

static void identify_fs(void)
{
    u8 *s = memalign(32, 4096);
    if (!s) return;
    memset(s, 0, 4096);
    u32 ss = __io_usbstorage_sector_size ? __io_usbstorage_sector_size : 512;
    if (!ro_read(0, 1, s)) { printf("Raw sector 0 read FAILED\n"); free(s); return; }
    printf("Boot signature 55AA: %s\n", (s[510] == 0x55 && s[511] == 0xAA) ? "yes" : "NO");
    if (!memcmp(s + 3, "EXFAT   ", 8)) {
        printf("Filesystem: exFAT (volume boot record at LBA0) - libfat can't mount\n");
    } else if (!memcmp(s + 0x52, "FAT32   ", 8)) {
        printf("Filesystem: FAT32 (superfloppy, no partition table)\n");
    } else if (!memcmp(s + 0x36, "FAT16   ", 8) || !memcmp(s + 0x36, "FAT12   ", 8)) {
        printf("Filesystem: %.8s (superfloppy)\n", (char *)s + 0x36);
    } else {
        printf("MBR partition table:\n");
        for (int i = 0; i < 4; i++) {
            u8 *p = s + 446 + i * 16;
            u32 start, size;
            memcpy(&start, p + 8, 4); memcpy(&size, p + 12, 4);
            start = __builtin_bswap32(start); size = __builtin_bswap32(size);  /* LE on disk */
            if (p[4]) printf("  P%d type=%02x start=%u size=%u (%.0f MiB)\n", i + 1, p[4],
                             start, size, (double)size * ss / 1048576.0);
        }
        printf("  (types 01/04/06/0E=FAT16/12, 0B/0C=FAT32, 07=exFAT/NTFS)\n");
    }
    free(s);
}

static bool mount_ro(void)
{
    page("3/4 Read-only FAT mount");
    printf("Starting USB mass storage via libogc...\n");
    if (!ro_startup()) { printf("ERROR: __io_usbstorage.startup() failed - no usable LUN.\n"); return false; }
    if (!ro_inserted()) { printf("ERROR: no media inserted.\n"); return false; }
    g_sector_size = __io_usbstorage_sector_size ? __io_usbstorage_sector_size : 512;
    printf("Sector size reported: %u B\n", g_sector_size);
    identify_fs();

    if (!fatMount("usb", &ro_usb, 0, 8, 64)) {
        printf("ERROR: libfat could not mount a FAT partition (exFAT/NTFS/unformatted?).\n");
        return false;
    }
    g_mounted = true;
    printf("Mounted as usb:/ READ-ONLY\n");

    struct statvfs sv;
    if (statvfs("usb:/", &sv) == 0) {
        double tot = (double)sv.f_blocks * sv.f_frsize, fre = (double)sv.f_bfree * sv.f_frsize;
        printf("FS capacity: %.1f MiB, free: %.1f MiB, used: %.1f MiB\n",
               tot / 1048576.0, fre / 1048576.0, (tot - fre) / 1048576.0);
        printf("Cluster size: %lu B\n", (unsigned long)sv.f_frsize);
    } else printf("statvfs failed\n");
    return true;
}

typedef struct { char path[256]; u32 size; } fent;
static fent files[MAX_FILES];
static int nfiles;

static void scan(const char *dir, int depth, bool print)
{
    DIR *d = opendir(dir);
    if (!d) { if (print) printf("  (cannot open %s)\n", dir); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[256];
        snprintf(p, sizeof(p), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (print && depth < 1) printf("%*s[%s]\n", depth * 2 + 1, "", e->d_name);
            if (depth < 4) scan(p, depth + 1, print && depth < 1);
        } else {
            if (print && depth < 1) printf("%*s%s (%lu B)\n", depth * 2 + 1, "", e->d_name, (unsigned long)st.st_size);
            if (nfiles < MAX_FILES) {
                strncpy(files[nfiles].path, p, 255);
                files[nfiles].size = (u32)st.st_size;
                nfiles++;
            }
        }
    }
    closedir(d);
}

static void list_files(void)
{
    printf("\nRoot listing (top level) :\n");
    nfiles = 0;
    scan("usb:/", 0, true);
    printf("\nFiles indexed for benchmark: %d\n", nfiles);
}

/* ---------------------------------------------------- 4. Benchmarks */
static u64 raw_read(u32 start, u32 nsec, u8 *buf, u32 *errs, u32 *crc)
{
    u64 bytes = 0;
    u32 done = 0;
    while (done < nsec) {
        u32 n = nsec - done > CHUNK_SEC ? CHUNK_SEC : nsec - done;
        if (!ro_read(start + done, n, buf)) { (*errs)++; done += n; continue; }
        if (crc) *crc = adler(buf, n * g_sector_size, *crc);
        bytes += (u64)n * g_sector_size;
        done += n;
    }
    return bytes;
}

static void bench(void)
{
    page("4/4 Read-only benchmarks");
    u8 *buf = memalign(32, CHUNK_SEC * 4096);
    if (!buf) { printf("OOM\n"); return; }

    /* usable raw region: first 32 MiB or card size, whichever is smaller */
    u32 region = (32u * 1024 * 1024) / g_sector_size;
    u8 *probe = buf;
    (void)probe;
    printf("[A] RAW sectors via USB (bypasses FAT + libfat cache)\n");
    printf("    reads LBA 0..%u only; no writes.\n", region - 1);

    u32 errs = 0;
    u64 t0 = gettime();
    u64 b = raw_read(0, region, buf, &errs, NULL);
    print_rate("seq 64K x N", b, ms_since(t0), errs);

    /* sustained: loop ~15 s, report per-second window */
    printf("\n[B] Sustained raw read, 15 s (window = 2 s)\n");
    u64 tstart = gettime(), twin = tstart, wbytes = 0, total = 0;
    u32 serr = 0, nwin = 0; double mn = 1e9, mx = 0;
    while (ms_since(tstart) < 15000) {
        u64 got = raw_read(0, region / 8, buf, &serr, NULL);
        wbytes += got; total += got;
        u32 wms = ms_since(twin);
        if (wms >= 2000) {
            double r = ((double)wbytes / 1048576.0) / (wms / 1000.0);
            if (r < mn) mn = r;
            if (r > mx) mx = r;
            printf("    window %u: %.2f MiB/s\n", ++nwin, r);
            wbytes = 0; twin = gettime();
        }
    }
    print_rate("sustained", total, ms_since(tstart), serr);
    if (nwin) printf("    min %.2f / max %.2f MiB/s over %u windows\n", mn, mx, nwin);

    /* stability: 5 passes over 4 MiB, compare checksums */
    printf("\n[C] Stability: 5 x 4 MiB raw reads, checksum compare\n");
    u32 first = 0, mism = 0, rerr = 0;
    for (int i = 0; i < 5; i++) {
        u32 crc = 1;
        raw_read(0, (4u * 1024 * 1024) / g_sector_size, buf, &rerr, &crc);
        if (i == 0) first = crc; else if (crc != first) mism++;
    }
    printf("    read errors=%u, checksum mismatches=%u\n", rerr, mism);
    wait_a("raw tests done");

    /* ------- file-level tests (FS + libfat cache + USB mixed) */
    page("4/4 File-level benchmarks");
    printf("[D] FILE reads through libfat (includes FS + cache effects)\n");
    int big = -1;
    for (int i = 0; i < nfiles; i++)
        if (files[i].size >= 1048576 && (big < 0 || files[i].size > files[big].size)) big = i;
    if (big < 0) printf("    no file >= 1 MiB on card; skipping large-file test\n");
    else {
        printf("    file: %s (%u B), first 16 MiB max\n", files[big].path, files[big].size);
        for (int pass = 1; pass <= 2; pass++) {
            FILE *f = fopen(files[big].path, "rb");
            if (!f) { printf("    open failed\n"); break; }
            u64 got = 0, cap = 16ull * 1024 * 1024; u32 e = 0; size_t n;
            u64 t = gettime();
            while (got < cap && (n = fread(buf, 1, CHUNK_SEC * 512, f)) > 0) got += n;
            if (ferror(f)) e++;
            fclose(f);
            print_rate(pass == 1 ? "file pass 1" : "file pass 2", got, ms_since(t), e);
        }
        printf("    pass2 >> pass1 => libfat cache, NOT USB speed\n");
    }

    u32 cnt = 0, serrs = 0; u64 sb = 0, ts = gettime();
    for (int i = 0; i < nfiles && cnt < 100; i++) {
        if (files[i].size == 0 || files[i].size > 65536) continue;
        FILE *f = fopen(files[i].path, "rb");
        if (!f) { serrs++; continue; }
        size_t n;
        while ((n = fread(buf, 1, 65536, f)) > 0) sb += n;
        if (ferror(f)) serrs++;
        fclose(f);
        cnt++;
    }
    u32 sms = ms_since(ts);
    printf("\n[E] Small files (<=64 KiB), %u files\n", cnt);
    print_rate("small files", sb, sms, serrs);
    if (sms && cnt) printf("    %.1f files/s (open+read+close)\n", cnt * 1000.0 / sms);
    free(buf);
}

/* Optional log save: creates ONE NEW file, never overwrites anything. */
static void save_log(void)
{
    page("Save log");
    printf("Save this session's log to the USB card?\n");
    printf("Creates a NEW file %s_N.txt at the card root.\n", LOG_NAME);
    printf("Existing files are never modified.\n");
    printf("\nA = save    B = skip\n");
    for (;;) {
        int d = poll_btn();
        if (d & BTN_HOME) quit();
        if (d & BTN_B) return;
        if (d & BTN_A) break;
        VIDEO_WaitVSync();
    }
    if (g_mounted) { fatUnmount("usb"); g_mounted = false; }
    if (!fatMountSimple("usbw", &__io_usbstorage)) { printf("Cannot mount USB read/write; log not saved.\n"); return; }
    char path[64];
    int n;
    for (n = 0; n < 100; n++) {
        snprintf(path, sizeof(path), "usbw:/%s_%d.txt", LOG_NAME, n);
        FILE *t = fopen(path, "rb");
        if (!t) break;
        fclose(t);
    }
    if (n == 100) { printf("100 logs already exist; not saving.\n"); fatUnmount("usbw"); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("Cannot create %s (card write-protected?)\n", path); fatUnmount("usbw"); return; }
    size_t w = fwrite(logbuf, 1, loglen, f);
    fclose(f);
    fatUnmount("usbw");
    printf("Saved %u of %u bytes to %s\n", (unsigned)w, (unsigned)loglen, path);
}

/* ------------------------------------------------------------- main */
int main(void)
{
    video_init();
    WPAD_Init();
    PAD_Init();

    page("Overview");
    printf("Read-only, except an optional log save you confirm at the end.\n");
    printf("IOS in use: %d (IOS58 recommended: provides /dev/usb/ven, USB 2.0)\n", IOS_GetVersion());
    printf("Plug the hub in BEFORE launching. Remove SD from the Wii front\n"
           "slot if you want unambiguous results (HBC may be booting from it).\n");
    wait_a("start");

    enumerate_usb();
    wait_a("next: LUN probe");
    probe_luns();
    wait_a("next: read-only mount");
    if (mount_ro()) {
        list_files();
        wait_a("next: benchmarks");
        bench();
        fatUnmount("usb");
        g_mounted = false;
    }
    save_log();
    ro_shutdown();
    wait_a("finished - HOME to exit");
    quit();
    return 0;
}
