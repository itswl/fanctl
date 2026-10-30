/*
 * fanctl - temperature-based fan control for Apple Silicon Macs, talking to the SMC directly.
 *
 *   fanctl status                  temperatures, fan speeds and modes, active preset, daemon
 *   fanctl start | stop            start the daemon now / stop it and hand the fans to macOS (root)
 *   fanctl autostart [on|off]      show / set whether it starts at boot          (root to set)
 *   fanctl preset [NAME]           list the presets / switch to one              (root)
 *   fanctl reload                  check the config and make the daemon reload it (root)
 *   fanctl temp                    the PMU temperature the curve uses
 *   fanctl set <rpm>               pin every fan to <rpm>                  (root)
 *   fanctl set <fan#> <rpm>        pin one fan                             (root)
 *   fanctl auto                    hand the fans back to the SMC           (root)
 *   fanctl check [--config FILE]   validate a config file
 *   fanctl run [--config FILE] [--dry-run] [--verbose]
 *                                  follow the curve; started by launchd    (root unless --dry-run)
 *   fanctl version
 *
 * Temperatures come from two places, for two jobs:
 *   curve  the maximum of the HID "PMU tdie*" sensors. This is not a CPU core temperature:
 *          it moves slowly (+3 °C after 25 s with five P-cores busy), so the fans stay steady.
 *   guard  CPU and GPU sensors in the SMC. They pass 85 °C within two seconds of load and are
 *          spiky, so they are smoothed and only push the fans up when the chip stays hot.
 * Fans are driven through the AppleSMC keys F<i>Md / F<i>Tg; writing needs root.
 *
 * Build: clang -O2 -o fanctl fanctl.c -framework IOKit -framework CoreFoundation
 */
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <spawn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

#define FANCTL_VERSION "1.0.3"

/* Install locations. The Homebrew formula builds with its own (see Formula/fanctl.rb). */
#ifndef CONF_PATH
#define CONF_PATH      "/usr/local/etc/fancontrol.conf"
#endif
#ifndef PRESET_DIR
#define PRESET_DIR     "/usr/local/share/fanctl/presets"
#endif
#ifndef LOG_PATH
#define LOG_PATH       "/var/log/fancontrol.log"
#endif
#ifndef SETUP_HINT
#define SETUP_HINT     "install fanctl with the one-line command in the README"
#endif
#define LOCK_PATH      "/var/run/fanctl.lock"
#ifndef LABEL
#define LABEL          "local.fancontrol"
#endif
#define PLIST_PATH     "/Library/LaunchDaemons/" LABEL ".plist"
#define MAX_FANS       8

/* While the fans are ours, the slow inputs are only re-read every few ticks. */
#define PMU_EVERY 2     /* PMU temperature: 20 HID calls, and it moves at most ~0.15 °C/s */
#define GPU_EVERY 5     /* GPU sensors: 18 SMC calls */
#define TG_EVERY  10    /* fan targets: only to notice another writer */

/* ------------------------------------------------------------------ log */

/* One write per line, timestamped (launchd sends stderr to the log file). */
static void logmsg(const char *fmt, ...)
{
    char line[512];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    size_t n = strftime(line, sizeof line, "%Y-%m-%d %H:%M:%S fanctl: ", &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    fputs(line, stderr);
}

/* ------------------------------------------------------------------ SMC */

typedef struct { char major, minor, build, reserved[1]; uint16_t release; } SMCVersion;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } SMCPLimitData;
typedef struct { uint32_t dataSize; uint32_t dataType; uint8_t dataAttributes; } SMCKeyInfoData;
/* Must match the kernel's 80-byte layout, implicit padding included. */
typedef struct {
    uint32_t key;
    SMCVersion vers;
    SMCPLimitData pLimit;
    SMCKeyInfoData keyInfo;
    uint8_t result;
    uint8_t status;
    uint8_t data8;
    uint32_t data32;
    uint8_t bytes[32];
} SMCKeyData;

#define KERNEL_INDEX_SMC     2
#define SMC_CMD_READ_BYTES   5
#define SMC_CMD_WRITE_BYTES  6
#define SMC_CMD_READ_INDEX   8
#define SMC_CMD_READ_KEYINFO 9
#define SMC_TYPE_FLT         0x666c7420u   /* 'flt ' */

static io_connect_t smc_conn = 0;

static uint32_t fourcc(const char *k)
{
    return ((uint32_t)(unsigned char)k[0] << 24) | ((uint32_t)(unsigned char)k[1] << 16) |
           ((uint32_t)(unsigned char)k[2] << 8) | (uint32_t)(unsigned char)k[3];
}

static int smc_open(void)
{
    if (smc_conn) return 0;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) { fprintf(stderr, "fanctl: AppleSMC service not found\n"); return -1; }
    kern_return_t r = IOServiceOpen(svc, mach_task_self(), 0, &smc_conn);
    IOObjectRelease(svc);
    if (r != kIOReturnSuccess) { fprintf(stderr, "fanctl: cannot open AppleSMC (0x%x)\n", r); return -1; }
    return 0;
}

static int smc_call(SMCKeyData *in, SMCKeyData *out)
{
    memset(out, 0, sizeof *out);
    size_t sz = sizeof *out;
    kern_return_t r = IOConnectCallStructMethod(smc_conn, KERNEL_INDEX_SMC, in, sizeof *in, out, &sz);
    return (r == kIOReturnSuccess && out->result == 0) ? 0 : -1;
}

/* Key info never changes, so it is cached: one kernel call per read instead of two. */
static struct { uint32_t key; SMCKeyInfoData info; } kinfo_cache[256];
static int kinfo_n = 0;

static int smc_keyinfo(uint32_t key, SMCKeyInfoData *info)
{
    for (int i = 0; i < kinfo_n; i++)
        if (kinfo_cache[i].key == key) { *info = kinfo_cache[i].info; return 0; }
    SMCKeyData in, out;
    memset(&in, 0, sizeof in);
    in.key = key;
    in.data8 = SMC_CMD_READ_KEYINFO;
    if (smc_call(&in, &out) != 0) return -1;
    *info = out.keyInfo;
    if (kinfo_n < (int)(sizeof kinfo_cache / sizeof kinfo_cache[0])) {
        kinfo_cache[kinfo_n].key = key;
        kinfo_cache[kinfo_n].info = out.keyInfo;
        kinfo_n++;
    }
    return 0;
}

static int smc_read(uint32_t key, SMCKeyInfoData *info, uint8_t bytes[32])
{
    SMCKeyData in, out;
    if (smc_keyinfo(key, info) != 0) return -1;
    memset(&in, 0, sizeof in);
    in.key = key;
    in.keyInfo = *info;
    in.data8 = SMC_CMD_READ_BYTES;
    if (smc_call(&in, &out) != 0) return -1;
    memcpy(bytes, out.bytes, 32);
    return 0;
}

/* Writes as many bytes as given; the mode keys are a single ui8 byte. */
static int smc_write_bytes(const char *key, const void *data, unsigned len)
{
    SMCKeyInfoData info;
    SMCKeyData in, out;
    uint32_t k = fourcc(key);
    if (smc_keyinfo(k, &info) != 0) return -1;
    if (len > info.dataSize || len > sizeof in.bytes) return -1;
    memset(&in, 0, sizeof in);
    in.key = k;
    in.keyInfo = info;
    in.data8 = SMC_CMD_WRITE_BYTES;
    memcpy(in.bytes, data, len);
    return smc_call(&in, &out);
}

static int smc_read_float_k(uint32_t key, float *value)
{
    SMCKeyInfoData info;
    uint8_t b[32];
    if (smc_read(key, &info, b) != 0) return -1;
    if (info.dataSize != 4 || info.dataType != SMC_TYPE_FLT) return -1;
    memcpy(value, b, 4);
    return 0;
}

static int smc_read_float(const char *key, float *value)
{
    return smc_read_float_k(fourcc(key), value);
}

static int smc_write_float(const char *key, float value)
{
    SMCKeyInfoData info;
    if (smc_keyinfo(fourcc(key), &info) != 0) return -1;
    if (info.dataSize != 4 || info.dataType != SMC_TYPE_FLT) return -1;
    return smc_write_bytes(key, &value, 4);
}

static int smc_read_u8(const char *key, int *value)
{
    SMCKeyInfoData info;
    uint8_t b[32];
    if (smc_read(fourcc(key), &info, b) != 0) return -1;
    if (info.dataSize != 1) return -1;
    *value = b[0];
    return 0;
}

static int fan_count(void)
{
    int n;
    return smc_read_u8("FNum", &n) == 0 ? n : 0;
}

/* ------------------------------------------------------------------ CPU / GPU temperature (SMC, guard) */

/* Every key read is a round trip into the kernel. Reading every Tp* (P-core), Te* (E-core) and
 * Tg* (GPU) key is 79 calls on an M3 Pro. TCMz, kept up to date by the SMC, equals the Tp*
 * maximum; when that checks out at start-up it stands in for all Tp/Te keys (E-cores always run
 * cooler than P-cores) and the GPU keys are only re-read every few ticks. Key names differ
 * between chip generations, so the keys are found by prefix rather than hard-coded. */
static uint32_t cpu_keys[192];      /* Tp/Te/Tg keys, or only Tg keys when TCMz is used */
static int cpu_n = -1;              /* -1 = not scanned yet */
static uint32_t cpu_agg = 0;        /* fourcc("TCMz") when usable, else 0 */
static double gpu_last = -1;

/* Maximum over a key list; c1 != 0 restricts it to keys whose second letter is c1. */
static double max_keys(const uint32_t *keys, int n, char c1)
{
    double mx = -1;
    for (int i = 0; i < n; i++) {
        if (c1 && (char)(keys[i] >> 16) != c1) continue;
        float v;
        if (smc_read_float_k(keys[i], &v) != 0) continue;
        if (v > 0 && v < 130 && v > mx) mx = v;
    }
    return mx;
}

static void cpu_scan(void)
{
    SMCKeyInfoData info;
    uint8_t b[32];
    cpu_n = 0;
    if (smc_read(fourcc("#KEY"), &info, b) != 0) return;
    uint32_t total = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    for (uint32_t i = 0; i < total && cpu_n < (int)(sizeof cpu_keys / sizeof cpu_keys[0]); i++) {
        SMCKeyData in, out;
        memset(&in, 0, sizeof in);
        in.data8 = SMC_CMD_READ_INDEX;
        in.data32 = i;
        if (smc_call(&in, &out) != 0) continue;
        uint32_t k = out.key;
        char c0 = (char)(k >> 24), c1 = (char)(k >> 16);
        if (c0 != 'T' || (c1 != 'p' && c1 != 'e' && c1 != 'g')) continue;
        if (smc_keyinfo(k, &info) != 0) continue;
        if (info.dataSize != 4 || info.dataType != SMC_TYPE_FLT) continue;
        cpu_keys[cpu_n++] = k;
    }

    /* Read TCMz before and after taking the Tp* maximum; use it only if the maximum falls
     * inside that bracket (±1 °C) in two of three tries. */
    uint32_t agg = fourcc("TCMz");
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        float a, z;
        if (smc_read_float_k(agg, &a) != 0) break;
        double p = max_keys(cpu_keys, cpu_n, 'p');
        if (smc_read_float_k(agg, &z) != 0) break;
        if (p > 0 && p >= fminf(a, z) - 1 && p <= fmaxf(a, z) + 1) ok++;
    }
    if (ok >= 2) {
        int m = 0;
        for (int i = 0; i < cpu_n; i++)
            if ((char)(cpu_keys[i] >> 16) == 'g') cpu_keys[m++] = cpu_keys[i];
        cpu_n = m;
        cpu_agg = agg;
    }
}

/* Hottest CPU/GPU sensor (instantaneous, spiky); -1 if nothing can be read.
 * refresh_gpu = 0 reuses the last GPU maximum. */
static double read_cpu_temp(int refresh_gpu)
{
    if (cpu_n < 0) cpu_scan();
    if (!cpu_agg) return max_keys(cpu_keys, cpu_n, 0);
    float z;
    double cpu = (smc_read_float_k(cpu_agg, &z) == 0 && z > 0 && z < 130) ? z : -1;
    if (refresh_gpu || gpu_last < 0) gpu_last = max_keys(cpu_keys, cpu_n, 0);
    return cpu > gpu_last ? cpu : gpu_last;
}

/* ------------------------------------------------------------------ PMU temperature (HID, curve) */

typedef struct __IOHIDEventSystemClient *HIDClient;
typedef struct __IOHIDServiceClient *HIDService;
typedef struct __IOHIDEvent *HIDEvent;

static HIDClient (*hid_create)(CFAllocatorRef);
static CFArrayRef (*hid_copyServices)(HIDClient);
static CFTypeRef (*hid_copyProperty)(HIDService, CFStringRef);
static HIDEvent (*hid_copyEvent)(HIDService, int64_t, int32_t, int64_t);
static double (*hid_getFloat)(HIDEvent, int32_t);
static HIDClient hid_client = NULL;
static CFMutableArrayRef hid_sensors = NULL;   /* the tdie (or tdev) services, picked once */

#define HID_EVENT_TEMPERATURE 15
#define HID_FIELD_TEMP_LEVEL  (15 << 16)

static int hid_load(void)
{
    if (hid_create) return 0;
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!h) return -1;
    hid_create = dlsym(h, "IOHIDEventSystemClientCreate");
    hid_copyServices = dlsym(h, "IOHIDEventSystemClientCopyServices");
    hid_copyProperty = dlsym(h, "IOHIDServiceClientCopyProperty");
    hid_copyEvent = dlsym(h, "IOHIDServiceClientCopyEvent");
    hid_getFloat = dlsym(h, "IOHIDEventGetFloatValue");
    if (!hid_create || !hid_copyServices || !hid_copyProperty || !hid_copyEvent || !hid_getFloat) {
        hid_create = NULL;
        return -1;
    }
    return 0;
}

static void hid_reset(void)
{
    if (hid_sensors) { CFRelease(hid_sensors); hid_sensors = NULL; }
    if (hid_client)  { CFRelease(hid_client);  hid_client = NULL; }
}

/* Picks the "PMU tdie*" services, or "PMU tdev*" when there are none.
 * Returns how many were picked, -1 on error. */
static int hid_build(void)
{
    if (hid_load() != 0) return -1;
    if (!hid_client) hid_client = hid_create(kCFAllocatorDefault);
    if (!hid_client) return -1;
    CFArrayRef svcs = hid_copyServices(hid_client);
    if (!svcs) return -1;

    CFMutableArrayRef die = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    CFMutableArrayRef dev = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    long n = CFArrayGetCount(svcs);
    for (long i = 0; i < n; i++) {
        HIDService s = (HIDService)CFArrayGetValueAtIndex(svcs, i);
        CFTypeRef p = hid_copyProperty(s, CFSTR("Product"));
        if (!p) continue;
        char name[128] = {0};
        if (CFGetTypeID(p) == CFStringGetTypeID())
            CFStringGetCString(p, name, sizeof name, kCFStringEncodingUTF8);
        CFRelease(p);
        if (strstr(name, "PMU tdie"))      CFArrayAppendValue(die, s);
        else if (strstr(name, "PMU tdev")) CFArrayAppendValue(dev, s);
    }
    CFRelease(svcs);
    if (CFArrayGetCount(die) > 0) { hid_sensors = die; CFRelease(dev); }
    else                          { hid_sensors = dev; CFRelease(die); }
    return (int)CFArrayGetCount(hid_sensors);
}

static double hid_read_max(void)
{
    double mx = -1;
    long n = CFArrayGetCount(hid_sensors);
    for (long i = 0; i < n; i++) {
        HIDService s = (HIDService)CFArrayGetValueAtIndex(hid_sensors, i);
        HIDEvent ev = hid_copyEvent(s, HID_EVENT_TEMPERATURE, 0, 0);
        if (!ev) continue;
        double t = hid_getFloat(ev, HID_FIELD_TEMP_LEVEL);
        CFRelease(ev);
        if (t > 0 && t < 200 && t > mx) mx = t;
    }
    return mx;
}

/* The curve temperature: max of PMU tdie (tdev on machines without tdie); -1 if unreadable. */
static double read_temp(void)
{
    if (!hid_sensors && hid_build() < 0) { hid_reset(); return -1; }
    double t = hid_read_max();
    if (t < 0) {                     /* stale services (e.g. after sleep): rebuild once */
        hid_reset();
        if (hid_build() > 0) t = hid_read_max();
    }
    return t;
}

/* ------------------------------------------------------------------ fans */

typedef struct { float actual, target, min, max; int mode; } FanState;   /* mode: 1 manual, 0 auto, -1 unknown */

static int fan_float(int idx, const char *suffix, float *v)
{
    char k[8];
    snprintf(k, sizeof k, "F%d%s", idx, suffix);
    return smc_read_float(k, v);
}

/* The mode key is F<i>Md, or F<i>md on newer chips; looked up once per fan. */
static char mode_keys[MAX_FANS][8];

static const char *mode_key(int idx)
{
    if (idx < 0 || idx >= MAX_FANS) return NULL;
    char *k = mode_keys[idx];
    if (!k[0]) {
        SMCKeyInfoData info;
        snprintf(k, 8, "F%dMd", idx);
        if (smc_keyinfo(fourcc(k), &info) != 0) {
            snprintf(k, 8, "F%dmd", idx);
            if (smc_keyinfo(fourcc(k), &info) != 0) strcpy(k, "-");
        }
    }
    return k[0] == '-' ? NULL : k;
}

static int fan_mode(int idx)
{
    const char *k = mode_key(idx);
    int m;
    return k && smc_read_u8(k, &m) == 0 ? m : -1;
}

static int read_fan(int idx, FanState *f)
{
    if (fan_float(idx, "Ac", &f->actual) || fan_float(idx, "Tg", &f->target) ||
        fan_float(idx, "Mn", &f->min) || fan_float(idx, "Mx", &f->max)) return -1;
    f->mode = fan_mode(idx);
    return 0;
}

/* Mode: 1 = manual, 0 = SMC, 3 = macOS is managing the temperature and refuses manual. It is a
 * one-byte ui8; written as a float, 1.0 would put 0x00 in the first byte. */
static int set_fan_mode(int idx, unsigned char manual)
{
    const char *k = mode_key(idx);
    return k ? smc_write_bytes(k, &manual, 1) : -1;
}

/* Ftst is one switch for all fans. Some MacBooks refuse manual mode (mode 3) until it is 1, and
 * while it is 1 macOS stops managing the temperature, so it goes back to 0 whenever the fans are
 * handed back. Most Macs do not have it and take manual mode directly. */
static int has_ftst(void)
{
    static int have = -1;
    SMCKeyInfoData info;
    if (have < 0) have = smc_keyinfo(fourcc("Ftst"), &info) == 0;
    return have;
}

static void ftst_off(void)
{
    int v;
    unsigned char off = 0;
    if (has_ftst() && smc_read_u8("Ftst", &v) == 0 && v == 1) smc_write_bytes("Ftst", &off, 1);
}

static int take_manual(int idx)
{
    if (set_fan_mode(idx, 1) == 0) return 0;
    if (!has_ftst()) return -1;
    unsigned char on = 1;
    if (smc_write_bytes("Ftst", &on, 1) != 0) return -1;
    usleep(500000);
    for (int i = 0; i < 100; i++) {                 /* up to ~10 s; usually 4-6 s */
        if (set_fan_mode(idx, 1) == 0) return 0;
        usleep(100000);
    }
    ftst_off();                                     /* never leave macOS sidelined for nothing */
    return -1;
}

/* A target written without manual mode is silently dropped (and without root the SMC also
 * answers "ok" and does nothing), so every write is read back. */
static int set_fan(int idx, float rpm)
{
    char k[8];
    if (take_manual(idx) != 0) return -1;
    usleep(20000);
    snprintf(k, sizeof k, "F%dTg", idx);
    if (smc_write_float(k, rpm) != 0) return -1;
    for (int i = 0; i < 15; i++) {          /* it takes 30-100 ms to land */
        usleep(100000);
        float got = 0;
        if (smc_read_float(k, &got) == 0 && fabsf(got - rpm) <= 1.0f) return 0;
    }
    return -1;
}

/* Hands every fan back to the SMC and reads the mode back; returns how many are still manual. */
static int restore_auto(void)
{
    int n = fan_count();
    for (int i = 0; i < n; i++) {
        char k[8];
        snprintf(k, sizeof k, "F%dTg", i);
        smc_write_float(k, 0.0f);       /* clear the target so the fan may stop */
        set_fan_mode(i, 0);
    }
    ftst_off();
    int still = 0;
    for (int tries = 0; tries < 10; tries++) {
        usleep(100000);
        still = 0;
        for (int i = 0; i < n; i++)
            if (fan_mode(i) == 1) still++;
        if (!still) break;
    }
    return still;
}

/* ------------------------------------------------------------------ config and curve */

typedef struct { double temp; double value; } CurvePoint;

typedef struct {
    CurvePoint curve[32];
    int curve_n;
    int curve_is_pct;       /* 0 = values are rpm, 1 = percent of each fan's range */
    int interval;           /* seconds between ticks while the fans are ours */
    double auto_below;      /* PMU temperature below which the SMC keeps the fans */
    double auto_hyst;       /* how far below auto_below before handing back */
    int confirm_up;         /* consecutive checks a rise must last before fanctl reacts */
    int confirm_down;       /* ... and a fall */
    int guard_on;
    double guard_start;     /* smoothed CPU temperature where the guard starts pushing */
    double guard_full;      /* ... and where it reaches 100 % */
} Config;

static int load_config(const char *path, Config *out)
{
    Config c;
    memset(&c, 0, sizeof c);
    c.interval = 3;
    c.auto_below = 52.0;
    c.auto_hyst = 4.0;
    c.confirm_up = 5;
    c.confirm_down = 20;
    c.guard_on = 1;
    c.guard_start = 95.0;
    c.guard_full = 108.0;

    FILE *fp = fopen(path, "r");
    if (!fp) { logmsg("cannot open %s: %s", path, strerror(errno)); return -1; }
    char line[256];
    int n_abs = 0, n_pct = 0, dropped = 0;
    while (fgets(line, sizeof line, fp)) {
        char *p = strchr(line, '#');
        if (p) *p = 0;
        line[strcspn(line, "\r\n")] = 0;
        char key[32];
        double a, b;
        if (sscanf(line, " curve_pct %lf %lf", &a, &b) == 2) {
            if (b < 0 || b > 100) {
                logmsg("curve_pct values must be 0-100: %s", line);
                fclose(fp);
                return -1;
            }
            if (c.curve_n < 32) { c.curve[c.curve_n].temp = a; c.curve[c.curve_n].value = b; c.curve_n++; }
            else dropped++;
            n_pct++;
        } else if (sscanf(line, " curve %lf %lf", &a, &b) == 2) {
            if (c.curve_n < 32) { c.curve[c.curve_n].temp = a; c.curve[c.curve_n].value = b; c.curve_n++; }
            else dropped++;
            n_abs++;
        } else if (sscanf(line, " interval %lf", &a) == 1) {
            c.interval = (int)a;
        } else if (sscanf(line, " auto_below %lf", &a) == 1) {
            c.auto_below = a;
        } else if (sscanf(line, " auto_hysteresis %lf", &a) == 1) {
            c.auto_hyst = a;
        } else if (sscanf(line, " confirm_up %lf", &a) == 1) {
            c.confirm_up = (int)a;
        } else if (sscanf(line, " confirm_down %lf", &a) == 1) {
            c.confirm_down = (int)a;
        } else if (sscanf(line, " cpu_guard %31s", key) == 1) {
            if (!strcmp(key, "off")) {
                c.guard_on = 0;
            } else if (sscanf(line, " cpu_guard %lf %lf", &a, &b) == 2 && b > a) {
                c.guard_on = 1;
                c.guard_start = a;
                c.guard_full = b;
            } else {
                logmsg("cpu_guard takes \"<start> <full>\" (full > start) or \"off\": %s", line);
                fclose(fp);
                return -1;
            }
        } else if (sscanf(line, " %31s", key) == 1) {
            logmsg("ignoring unknown setting: %s", key);
        }
    }
    fclose(fp);
    if (n_abs && n_pct) { logmsg("use either curve or curve_pct, not both"); return -1; }
    if (c.curve_n < 2) { logmsg("need at least two curve / curve_pct points"); return -1; }
    if (dropped) logmsg("only 32 curve points are used, %d ignored", dropped);
    c.curve_is_pct = n_pct > 0;
    for (int i = 1; i < c.curve_n; i++) {           /* insertion sort by temperature */
        CurvePoint tmp = c.curve[i];
        int j = i - 1;
        while (j >= 0 && c.curve[j].temp > tmp.temp) { c.curve[j + 1] = c.curve[j]; j--; }
        c.curve[j + 1] = tmp;
    }
    for (int i = 1; i < c.curve_n; i++)
        if (c.curve[i].value < c.curve[i - 1].value)
            logmsg("note: the curve goes down at %.0f°C (%.0f -> %.0f)", c.curve[i].temp,
                   c.curve[i - 1].value, c.curve[i].value);
    if (c.interval < 1) c.interval = 1;
    if (c.confirm_up < 1) c.confirm_up = 1;
    if (c.confirm_down < 1) c.confirm_down = 1;
    *out = c;
    return 0;
}

static void describe_config(const Config *c, char *buf, size_t n)
{
    char g[40];
    if (c->guard_on) snprintf(g, sizeof g, "%.0f-%.0f°C", c->guard_start, c->guard_full);
    else             snprintf(g, sizeof g, "off");
    snprintf(buf, n, "interval=%ds auto_below=%.0f°C hysteresis=%.0f confirm=%d up/%d down "
             "curve=%d points (%s) cpu_guard=%s", c->interval, c->auto_below, c->auto_hyst,
             c->confirm_up, c->confirm_down, c->curve_n, c->curve_is_pct ? "percent" : "rpm", g);
}

/* Linear interpolation; flat outside the first and last points. */
static double curve_value(const Config *c, double t)
{
    if (t <= c->curve[0].temp) return c->curve[0].value;
    for (int i = 1; i < c->curve_n; i++) {
        if (t <= c->curve[i].temp) {
            double span = c->curve[i].temp - c->curve[i - 1].temp;
            double k = span > 0 ? (t - c->curve[i - 1].temp) / span : 1.0;
            return c->curve[i - 1].value + k * (c->curve[i].value - c->curve[i - 1].value);
        }
    }
    return c->curve[c->curve_n - 1].value;
}

static double curve_rpm(const Config *c, double cv, double min, double max)
{
    double v = c->curve_is_pct ? min + cv / 100.0 * (max - min) : cv;
    return v < min ? min : (v > max ? max : v);
}

/* Guard: percent of the fan range once the smoothed CPU temperature passes guard_start;
 * -1 = not engaged. */
static double guard_pct(const Config *c, double cpu)
{
    if (!c->guard_on || cpu < c->guard_start) return -1;
    if (cpu >= c->guard_full) return 100;
    return (cpu - c->guard_start) / (c->guard_full - c->guard_start) * 100.0;
}

/* While the SMC has the fans there is nothing to do until the PMU temperature reaches
 * auto_below. It rises ~0.15 °C/s at most (+3 °C in 25 s with five P-cores busy); assume
 * 0.5 °C/s and sleep accordingly, between `interval` and 30 s. */
static int auto_sleep(const Config *c, double t)
{
    int s = (int)((c->auto_below - t) / 0.5);
    if (s < c->interval) s = c->interval;
    return s > 30 ? 30 : s;
}

/* ------------------------------------------------------------------ files, lock, daemon */

/* Reads a whole small file; returns its length, or -1. */
static long slurp(const char *path, char *buf, size_t cap)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    size_t n = fread(buf, 1, cap, fp);
    int big = !feof(fp);
    fclose(fp);
    return big ? -1 : (long)n;
}

/* Name of the preset the config is identical to, "custom", or "missing". */
static void preset_of(const char *conf, char *out, size_t outsz)
{
    static char a[16384], b[16384];
    long na = slurp(conf, a, sizeof a);
    snprintf(out, outsz, na < 0 ? "missing" : "custom");
    DIR *d = na < 0 ? NULL : opendir(PRESET_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".conf")) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", PRESET_DIR, e->d_name);
        if (slurp(p, b, sizeof b) == na && !memcmp(a, b, (size_t)na)) {
            snprintf(out, outsz, "%.*s", (int)(l - 5), e->d_name);
            break;
        }
    }
    closedir(d);
}

/* Only one `run` may drive the fans. Returns the fd; the lock goes away with the process. */
static int take_lock(void)
{
    int fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { logmsg("cannot open %s: %s", LOCK_PATH, strerror(errno)); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        logmsg("another fanctl run holds %s, not starting", LOCK_PATH);
        close(fd);
        return -1;
    }
    char buf[16];
    int len = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    if (ftruncate(fd, 0) == 0) { ssize_t w = write(fd, buf, len); (void)w; }
    return fd;
}

/* PID of the running daemon (from the lock file), 0 if none. */
static int daemon_pid(void)
{
    int fd = open(LOCK_PATH, O_RDONLY);
    if (fd < 0) return 0;
    int pid = 0;
    if (flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        char buf[16] = {0};
        ssize_t r = read(fd, buf, sizeof buf - 1);
        pid = r > 0 ? atoi(buf) : -1;
    }
    close(fd);
    return pid;
}

/* ------------------------------------------------------------------ commands */

static int autostart_state(void);

static int cmd_status(void)
{
    if (smc_open() != 0) return 1;
    int n = fan_count();
    double t = read_temp(), cpu = read_cpu_temp(1);
    char ts[16] = "n/a", cs[16] = "n/a";
    if (t > 0)   snprintf(ts, sizeof ts, "%.1f°C", t);
    if (cpu > 0) snprintf(cs, sizeof cs, "%.1f°C", cpu);
    printf("PMU %s (curve)   CPU %s (guard, instantaneous)   %d fan%s\n", ts, cs, n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {
        FanState f;
        if (read_fan(i, &f) != 0) { printf("  fan%d  unreadable\n", i); continue; }
        const char *m = f.mode == 1 ? "manual" : (f.mode == 0 || f.mode == 3) ? (f.target < 1 ? "auto (stopped)" : "auto") : "?";
        printf("  fan%d  %4.0f rpm  target %4.0f  range %.0f-%.0f  %s\n", i, f.actual, f.target, f.min, f.max, m);
    }
    char preset[64];
    preset_of(CONF_PATH, preset, sizeof preset);
    printf("config  %s (preset: %s)\n", CONF_PATH, preset);
    int pid = daemon_pid();
    if (pid > 0)       printf("daemon  running, pid %d\n", pid);
    else if (pid < 0)  printf("daemon  running\n");
    else               printf("daemon  not running (sudo fanctl start)\n");
    int a = autostart_state();
    printf("boot    %s\n", a > 0 ? "starts at boot (autostart on)" : a == 0 ? "does not start at boot (autostart off)" : "unknown");
    return 0;
}

static int cmd_temp(void)
{
    double t = read_temp();
    if (t < 0) { fprintf(stderr, "fanctl: cannot read the temperature\n"); return 1; }
    printf("%.1f\n", t);
    return 0;
}

static int parse_num(const char *s, double *v)
{
    char *end;
    errno = 0;
    *v = strtod(s, &end);
    return (end != s && *end == 0 && errno == 0) ? 0 : -1;
}

static void warn_daemon(void)
{
    if (daemon_pid())
        fprintf(stderr, "note: the fanctl daemon is running and will undo this within seconds; "
                        "stop it first: sudo fanctl stop\n");
}

static int cmd_set(int argc, char **argv)
{
    double idx_d = -1, rpm = 0;
    if (!(argc == 1 && parse_num(argv[0], &rpm) == 0) &&
        !(argc == 2 && parse_num(argv[0], &idx_d) == 0 && parse_num(argv[1], &rpm) == 0)) {
        fprintf(stderr, "usage: fanctl set <rpm> | fanctl set <fan#> <rpm>\n");
        return 2;
    }
    if (rpm <= 0) { fprintf(stderr, "fanctl: rpm must be > 0 (use 'fanctl auto' to hand back)\n"); return 2; }
    if (smc_open() != 0) return 1;
    int n = fan_count();
    int idx = -1;
    if (argc == 2) {
        if (idx_d < 0 || idx_d >= n || idx_d != floor(idx_d)) {
            fprintf(stderr, "fanctl: there is no fan%s (this Mac has %d, numbered from 0)\n", argv[0], n);
            return 2;
        }
        idx = (int)idx_d;
    }
    if (geteuid() != 0) { fprintf(stderr, "fanctl: writing to the SMC needs root: sudo fanctl set ...\n"); return 1; }
    warn_daemon();

    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (idx >= 0 && i != idx) continue;
        FanState f;
        if (read_fan(i, &f) != 0) { fprintf(stderr, "fan%d: cannot read\n", i); bad++; continue; }
        float v = rpm < f.min ? f.min : (rpm > f.max ? f.max : (float)rpm);
        if (set_fan(i, v) != 0) { fprintf(stderr, "fan%d: writing %.0f rpm failed\n", i, v); bad++; }
        else printf("fan%d -> %.0f rpm\n", i, v);
    }
    if (bad) { restore_auto(); fprintf(stderr, "fanctl: the fans are back with macOS\n"); }
    return bad ? 1 : 0;
}

static int cmd_auto(void)
{
    if (geteuid() != 0) { fprintf(stderr, "fanctl: writing to the SMC needs root: sudo fanctl auto\n"); return 1; }
    if (smc_open() != 0) return 1;
    warn_daemon();
    int still = restore_auto();
    int n = fan_count();
    for (int i = 0; i < n; i++) {
        FanState f;
        if (read_fan(i, &f) == 0)
            printf("fan%d: %s, target %.0f\n", i, f.mode == 1 ? "still manual" : "auto", f.target);
    }
    return still ? 1 : 0;
}

/* ------------------------------------------------------------------ daemon control */

extern char **environ;

/* Runs launchctl with up to three arguments, output discarded; returns its exit status. */
static int launchctl(const char *a, const char *b, const char *c)
{
    char *argv[] = { "launchctl", (char *)a, (char *)b, (char *)c, NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int st = -1;
    if (posix_spawn(&pid, "/bin/launchctl", &fa, NULL, argv, environ) == 0)
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    posix_spawn_file_actions_destroy(&fa);
    return (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
}

static int job_loaded(void) { return launchctl("print", "system/" LABEL, NULL) == 0; }

static int need_root(const char *cmd)
{
    if (geteuid() == 0) return 0;
    fprintf(stderr, "fanctl: %s needs root: sudo fanctl %s\n", cmd, cmd);
    return -1;
}

static void print_daemon_line(void)
{
    char preset[64];
    preset_of(CONF_PATH, preset, sizeof preset);
    int pid = daemon_pid(), a = autostart_state();
    const char *boot = a > 0 ? "on" : a == 0 ? "off" : "unknown";
    if (pid > 0) printf("fanctl is running (pid %d), preset: %s, autostart: %s\n", pid, preset, boot);
    else         printf("fanctl is not running, preset: %s, autostart: %s\n", preset, boot);
}

/* Starts at boot: the plist is installed and launchd has no "disabled" override for it.
 * Returns 1 / 0, or -1 when launchctl cannot be asked. */
static int autostart_state(void)
{
    if (access(PLIST_PATH, F_OK) != 0) return 0;
    FILE *p = popen("/bin/launchctl print-disabled system 2>/dev/null", "r");
    if (!p) return -1;
    char line[256];
    int disabled = 0;
    while (fgets(line, sizeof line, p))      /* "label" => disabled (newer) or => true (older) */
        if (strstr(line, "\"" LABEL "\"") && (strstr(line, "=> disabled") || strstr(line, "=> true")))
            disabled = 1;
    pclose(p);
    return !disabled;
}

static int cmd_autostart(int argc, char **argv)
{
    if (argc == 0) {
        int a = autostart_state();
        printf("autostart %s\n", a > 0 ? "on: fanctl starts at boot" : a == 0 ? "off: fanctl does not start at boot" : "unknown");
        return 0;
    }
    int on = argc == 1 && !strcmp(argv[0], "on"), off = argc == 1 && !strcmp(argv[0], "off");
    if (!on && !off) { fprintf(stderr, "usage: fanctl autostart [on|off]\n"); return 2; }
    if (geteuid() != 0) { fprintf(stderr, "fanctl: autostart needs root: sudo fanctl autostart %s\n", argv[0]); return 1; }
    if (on && access(PLIST_PATH, R_OK) != 0) {
        fprintf(stderr, "fanctl: %s is missing; " SETUP_HINT "\n", PLIST_PATH);
        return 1;
    }
    launchctl(on ? "enable" : "disable", "system/" LABEL, NULL);
    if (autostart_state() != on) { fprintf(stderr, "fanctl: launchctl did not take the change\n"); return 1; }
    if (on) printf("autostart on: fanctl starts at boot%s\n", daemon_pid() ? "" : " (it is not running now: sudo fanctl start)");
    else    printf("autostart off: fanctl will not start at boot%s\n", daemon_pid() ? " (it keeps running now: sudo fanctl stop)" : "");
    return 0;
}

static int cmd_start(void)
{
    if (need_root("start") != 0) return 1;
    if (access(PLIST_PATH, R_OK) != 0) {
        fprintf(stderr, "fanctl: %s is missing; " SETUP_HINT "\n", PLIST_PATH);
        return 1;
    }
    Config c;
    if (load_config(CONF_PATH, &c) != 0) return 1;
    /* launchd will not load a disabled job: with autostart off, enable it just long enough
     * to start it now, then disable it again so the boot setting stays as it was. */
    int was_off = autostart_state() == 0;
    launchctl("enable", "system/" LABEL, NULL);
    int i;
    for (i = 0; !job_loaded() && launchctl("bootstrap", "system", PLIST_PATH) != 0; i++) {
        if (i == 5) break;
        sleep(1);
    }
    if (was_off) launchctl("disable", "system/" LABEL, NULL);
    if (i == 5) { fprintf(stderr, "fanctl: launchctl bootstrap failed\n"); return 1; }
    for (int i = 0; daemon_pid() == 0; i++) {
        if (i == 40) { fprintf(stderr, "fanctl: the daemon did not start; see " LOG_PATH "\n"); return 1; }
        usleep(250000);
    }
    print_daemon_line();
    return 0;
}

/* Stops the daemon now; whether it starts at boot is left to `fanctl autostart`. */
static int cmd_stop(void)
{
    if (need_root("stop") != 0) return 1;
    if (job_loaded()) {
        launchctl("bootout", "system/" LABEL, NULL);
        for (int i = 0; i < 80 && job_loaded(); i++) usleep(250000);   /* bootout returns early */
    }
    int still = smc_open() == 0 ? restore_auto() : 1;   /* the daemon does this too; make sure */
    if (still) { fprintf(stderr, "fanctl: stopped, but %d fan(s) still read as manual; try: sudo fanctl auto\n", still); return 1; }
    printf("fanctl stopped, the fans are back with macOS.%s\n", autostart_state() > 0 ?
           " It starts again at boot (to keep it off: sudo fanctl autostart off)" : "");
    return 0;
}

static int cmd_reload(void)
{
    if (need_root("reload") != 0) return 1;
    Config c;
    if (load_config(CONF_PATH, &c) != 0) { fprintf(stderr, "fanctl: not reloaded, the daemon keeps the old config\n"); return 1; }
    if (daemon_pid() == 0) { printf("fanctl is not running; start it with: sudo fanctl start\n"); return 1; }
    if (launchctl("kill", "HUP", "system/" LABEL) != 0) { fprintf(stderr, "fanctl: could not signal the daemon\n"); return 1; }
    char d[200];
    describe_config(&c, d, sizeof d);
    printf("reloaded: %s\n", d);
    return 0;
}

/* Copies a whole file; returns 0 on success. Writes to a temporary file first. */
static int copy_file(const char *from, const char *to)
{
    static char buf[16384];
    long n = slurp(from, buf, sizeof buf);
    if (n < 0) return -1;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    int ok = fwrite(buf, 1, (size_t)n, fp) == (size_t)n;
    ok = (fclose(fp) == 0) && ok;
    if (!ok || chmod(tmp, 0644) != 0 || rename(tmp, to) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int cmd_preset(int argc, char **argv)
{
    char active[64];
    preset_of(CONF_PATH, active, sizeof active);
    if (argc == 0) {
        struct dirent **list;
        int m = scandir(PRESET_DIR, &list, NULL, alphasort);
        if (m < 0) { fprintf(stderr, "fanctl: no presets in %s\n", PRESET_DIR); return 1; }
        for (int i = 0; i < m; i++) {
            const char *f = list[i]->d_name;
            size_t l = strlen(f);
            if (l >= 6 && !strcmp(f + l - 5, ".conf")) {
                int on = !strncmp(active, f, l - 5) && active[l - 5] == 0;
                printf("%s %.*s\n", on ? "*" : " ", (int)(l - 5), f);
            }
            free(list[i]);
        }
        free(list);
        if (!strcmp(active, "custom")) printf("  (the config is custom: it matches no preset)\n");
        return 0;
    }
    if (argc != 1) { fprintf(stderr, "usage: fanctl preset [NAME]\n"); return 2; }
    const char *name = argv[0];
    if (!*name || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(name)) {
        fprintf(stderr, "fanctl: bad preset name: %s\n", name);
        return 2;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s.conf", PRESET_DIR, name);
    if (access(path, R_OK) != 0) { fprintf(stderr, "fanctl: no preset '%s' (list them with: fanctl preset)\n", name); return 2; }
    if (need_root("preset") != 0) return 1;
    Config c;
    if (load_config(path, &c) != 0) return 1;
    if (!strcmp(active, "custom") && copy_file(CONF_PATH, CONF_PATH ".bak") == 0)
        printf("your edited config was saved as %s.bak\n", CONF_PATH);
    if (copy_file(path, CONF_PATH) != 0) { fprintf(stderr, "fanctl: cannot write %s: %s\n", CONF_PATH, strerror(errno)); return 1; }
    if (daemon_pid() != 0 && launchctl("kill", "HUP", "system/" LABEL) == 0)
        printf("preset %s is active\n", name);
    else
        printf("preset %s is set; fanctl is not running (sudo fanctl start)\n", name);
    return 0;
}

static int cmd_check(int argc, char **argv)
{
    const char *path = CONF_PATH;
    if (argc == 2 && !strcmp(argv[0], "--config")) path = argv[1];
    else if (argc != 0) { fprintf(stderr, "usage: fanctl check [--config FILE]\n"); return 2; }
    Config c;
    if (load_config(path, &c) != 0) return 1;
    char d[200];
    describe_config(&c, d, sizeof d);
    printf("%s: ok, %s\n", path, d);
    return 0;
}

/* Signals: TERM / INT / HUP are set to SIG_IGN and collected through kqueue (EVFILT_SIGNAL).
 * Handing the fans back stays in the main loop (IOKit calls are not async-signal-safe), the
 * loop wakes once per tick instead of polling for signals, and a signal that arrives while a
 * tick is running is kept by kqueue and returned by the next wait. */
static int stop_sig = 0, reload_req = 0;
static int sig_kq = -1;

static int signals_init(void)
{
    int sigs[] = { SIGTERM, SIGINT, SIGHUP };
    sig_kq = kqueue();
    if (sig_kq < 0) return -1;
    for (int i = 0; i < 3; i++) {
        struct kevent ev;
        EV_SET(&ev, sigs[i], EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
        if (kevent(sig_kq, &ev, 1, NULL, 0, NULL) < 0) return -1;
        signal(sigs[i], SIG_IGN);
    }
    return 0;
}

/* Sleep and wake. fanctl never holds sleep off: before the Mac sleeps it hands the fans back to
 * the SMC, so a sleeping Mac's fans behave exactly as without fanctl, and only then lets it sleep;
 * after waking it starts over from SMC control. The notifications arrive on a Mach port that sits
 * in the same kqueue as the signals, so they are handled on the main thread between ticks. */
static io_connect_t pm_root = MACH_PORT_NULL;
static IONotificationPortRef pm_port = NULL;
static mach_port_t pm_set = MACH_PORT_NULL;
static int fans_ours = 0;           /* mirrors the main loop's "manual" for the sleep handler */
static int run_dry = 0;
static int power_event = 0;         /* slept or woke: the main loop starts over */

static void on_power(void *ref, io_service_t svc, natural_t type, void *arg)
{
    (void)ref;
    (void)svc;
    if (type == kIOMessageCanSystemSleep) {
        IOAllowPowerChange(pm_root, (intptr_t)arg);         /* never veto idle sleep */
    } else if (type == kIOMessageSystemWillSleep || type == kIOMessageSystemHasPoweredOn) {
        int had = fans_ours, still = 0;
        if (had && !run_dry) still = restore_auto();        /* also on a wake we did not see coming */
        fans_ours = 0;
        power_event = 1;
        if (type == kIOMessageSystemWillSleep) {
            IOAllowPowerChange(pm_root, (intptr_t)arg);
            logmsg("going to sleep%s", !had ? "" : still ? ": could not hand every fan back to SMC" : ": fans back to SMC");
        } else {
            logmsg("woke up: the SMC has the fans until a takeover is confirmed again");
        }
    }
}

static int power_init(void)
{
    io_object_t notifier;
    pm_root = IORegisterForSystemPower(NULL, &pm_port, on_power, &notifier);
    if (pm_root == MACH_PORT_NULL) return -1;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &pm_set) != KERN_SUCCESS ||
        mach_port_insert_member(mach_task_self(), IONotificationPortGetMachPort(pm_port), pm_set) != KERN_SUCCESS)
        return -1;
    struct kevent ev;
    EV_SET(&ev, pm_set, EVFILT_MACHPORT, EV_ADD, 0, 0, NULL);
    return kevent(sig_kq, &ev, 1, NULL, 0, NULL) < 0 ? -1 : 0;
}

/* Waits sec seconds, or until a signal or a sleep/wake notification arrives. */
static void wait_tick(int sec)
{
    struct timespec ts = { sec, 0 };
    struct kevent ev;
    int n = kevent(sig_kq, NULL, 0, &ev, 1, &ts);
    if (n < 0) { sleep(sec); return; }          /* should not happen; never spin */
    if (n == 0) return;
    if (ev.filter == EVFILT_MACHPORT) {
        union { mach_msg_header_t h; char buf[4096]; } m;
        if (mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, pm_set, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS)
            IODispatchCalloutFromMessage(NULL, &m.h, pm_port);
    } else if (ev.ident == SIGHUP) {
        reload_req = 1;
    } else {
        stop_sig = (int)ev.ident;
    }
}

/* How fast a fan level moves once a change is confirmed, as a share of the fan's range per
 * second: about 50 s from minimum to maximum, about 3 minutes back down. */
#define RAMP_UP   0.02
#define RAMP_DOWN 0.005

/* Smoothed CPU temperature at which the fans go straight back to macOS (Apple Silicon hot spots
 * sit at 95-103 °C under ordinary sustained load; the junction limit is about 110 °C), and how
 * long fanctl then stays out. Overridable at build time for tests only. */
#ifndef HOT_LIMIT
#define HOT_LIMIT 110.0
#endif
#ifndef COOLDOWN
#define COOLDOWN 120
#endif

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmd_run(int argc, char **argv)
{
    int dry = 0, verbose = 0;
    const char *path = CONF_PATH;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) path = argv[++i];
        else { fprintf(stderr, "fanctl: unknown option %s\n", argv[i]); return 2; }
    }
    Config cfg;
    if (load_config(path, &cfg) != 0) return 1;
    if (!dry && geteuid() != 0) {
        fprintf(stderr, "fanctl: writing to the SMC needs root (sudo, or let launchd start it)\n");
        return 1;
    }
    if (smc_open() != 0) return 1;

    int n = fan_count();
    if (n <= 0) { logmsg("no controllable fans (FNum = 0), nothing to do"); return 1; }
    if (n > MAX_FANS) n = MAX_FANS;
    float rmin[MAX_FANS], rmax[MAX_FANS], fset[MAX_FANS];    /* limits are read once */
    for (int i = 0; i < n; i++) {
        FanState f;
        if (read_fan(i, &f) != 0) { logmsg("cannot read fan%d", i); return 1; }
        rmin[i] = f.min;
        rmax[i] = f.max;
        fset[i] = -1;                   /* the target we last wrote; -1 = none */
    }
    double t = read_temp();
    if (t < 0) { logmsg("no temperature sensors (not an Apple Silicon Mac?), exiting"); return 1; }
    if (!dry && take_lock() < 0) return 1;
    if (signals_init() != 0) { logmsg("kqueue: %s", strerror(errno)); return 1; }
    run_dry = dry;
    if (power_init() != 0) logmsg("cannot watch for sleep; the fans will not be handed back before it");

    if (!dry) restore_auto();           /* start from SMC control; take over when it gets warm */
    char d[200];
    describe_config(&cfg, d, sizeof d);
    logmsg("%s %s: %s", dry ? "dry run" : "started", FANCTL_VERSION, d);

    int manual = 0;                     /* are the fans ours */
    int k = 0;                          /* checks since taking over */
    int bad_temp = 0;                   /* consecutive failed temperature reads */
    int up_n = 0, down_n = 0;           /* consecutive checks above auto_below / below the hand-back point */
    double cpu_ema = -1;
    double lvl[MAX_FANS];               /* the speed fanctl holds each fan at (rpm) */
    int rise_n[MAX_FANS], fall_n[MAX_FANS];
    int moving[MAX_FANS];               /* +1 ramping up, -1 ramping down, 0 holding */
    int cpu_bad = 0;                    /* consecutive checks without a CPU temperature */
    int take_fail = 0;                  /* consecutive checks where no fan could be taken over */
    time_t cooldown = 0;                /* no takeover before this time */
    double last_tick = 0;
    time_t last_write = 0, last_fail_log = 0;

    while (!stop_sig) {
        if (power_event) {              /* slept or woke: the fans are the SMC's again, start over */
            power_event = 0;
            manual = 0;
            up_n = down_n = 0;
        }
        if (reload_req) {               /* sudo launchctl kill HUP system/local.fancontrol */
            reload_req = 0;
            Config nc;
            if (load_config(path, &nc) == 0) {
                cfg = nc;
                describe_config(&cfg, d, sizeof d);
                logmsg("config reloaded: %s", d);
            } else {
                logmsg("the new config is invalid, keeping the old one");
            }
        }

        /* The PMU temperature is read on every check while the SMC has the fans; while they are
         * ours, every PMU_EVERY checks, or every check while a hand-back is being confirmed. */
        int fresh = 0;
        if (!manual || down_n > 0 || k % PMU_EVERY == 0) {
            double nt = read_temp();
            if (nt < 0) {
                /* Never sit on manual mode blind: after 3 failed reads the SMC gets the fans. */
                if (++bad_temp == 1) logmsg("cannot read the temperature");
                if (manual && bad_temp >= 3) {
                    manual = fans_ours = 0;
                    up_n = down_n = 0;
                    if (!dry) restore_auto();
                    logmsg("no temperature for %d reads -> fans back to SMC", bad_temp);
                }
                wait_tick(cfg.interval);
                continue;
            }
            if (bad_temp) { logmsg("temperature readable again: %.1f°C", nt); bad_temp = 0; }
            t = nt;
            fresh = 1;
        }

        if (!manual) {
            /* One high reading is not enough: take over only after confirm_up checks in a row at
             * or above auto_below. Meanwhile the SMC keeps cooling as usual. */
            up_n = t >= cfg.auto_below && time(NULL) >= cooldown ? up_n + 1 : 0;
            if (up_n < cfg.confirm_up) {
                int s = up_n > 0 ? cfg.interval : auto_sleep(&cfg, t);
                if (dry && verbose) {
                    if (up_n > 0)
                        printf("[dry] PMU %.1f°C >= %.0f°C, check %d of %d before taking over\n",
                               t, cfg.auto_below, up_n, cfg.confirm_up);
                    else if (time(NULL) < cooldown)
                        printf("[dry] PMU %.1f°C, SMC in charge, staying out for another %lds\n",
                               t, (long)(cooldown - time(NULL)));
                    else
                        printf("[dry] PMU %.1f°C, SMC in charge (takes over at %.0f°C), next check in %ds\n",
                               t, cfg.auto_below, s);
                }
                wait_tick(s);
                continue;
            }
            /* Start from wherever the SMC has the fans, so taking over is not a jump. */
            manual = fans_ours = 1;
            k = 0;
            up_n = down_n = 0;
            cpu_ema = -1;
            last_tick = 0;
            cpu_bad = 0;
            for (int i = 0; i < n; i++) {
                float ac;
                if (fan_float(i, "Ac", &ac) != 0) ac = rmin[i];
                lvl[i] = ac < rmin[i] ? rmin[i] : (ac > rmax[i] ? rmax[i] : ac);
                rise_n[i] = fall_n[i] = 0;
                moving[i] = 1;          /* head for the curve right away: the rise was confirmed */
                fset[i] = -1;
            }
            logmsg("PMU %.1f°C for %d checks (>= %.0f°C) -> taking over the fans", t, cfg.confirm_up, cfg.auto_below);
        } else if (fresh) {
            /* Hand back only after confirm_down checks in a row below auto_below - hysteresis. */
            down_n = t < cfg.auto_below - cfg.auto_hyst ? down_n + 1 : 0;
            if (down_n >= cfg.confirm_down) {
                manual = fans_ours = 0;
                up_n = down_n = 0;
                int still = dry ? 0 : restore_auto();
                if (still) logmsg("PMU %.1f°C -> fans back to SMC, but %d still read as manual", t, still);
                else       logmsg("PMU %.1f°C for %d checks (< %.0f°C) -> fans back to SMC", t,
                                  cfg.confirm_down, cfg.auto_below - cfg.auto_hyst);
                wait_tick(auto_sleep(&cfg, t));
                continue;
            }
        }

        double now = mono_now();
        double dt = last_tick > 0 ? now - last_tick : cfg.interval;
        if (dt > 2.0 * cfg.interval) dt = 2.0 * cfg.interval;      /* e.g. after sleep: no leaps */
        last_tick = now;

        /* The CPU temperature is spiky (the maximum jumps between 65 and 80 °C at idle), so the
         * guard works on an average with a 10 s time constant. */
        double cpu = read_cpu_temp(k % GPU_EVERY == 0);
        if (cpu > 0) cpu_ema = cpu_ema < 0 ? cpu : cpu_ema + (1 - exp(-dt / 10.0)) * (cpu - cpu_ema);
        cpu_bad = cpu > 0 ? 0 : cpu_bad + 1;

        /* The two exceptions to confirming hand the fans straight back to macOS and keep fanctl
         * out for two minutes: the CPU at its limit (macOS then also throttles, and on Macs that
         * needed Ftst its own temperature management resumes), and CPU sensors gone blind. */
        if (cpu_ema >= HOT_LIMIT || cpu_bad >= 3) {
            manual = fans_ours = 0;
            up_n = down_n = 0;
            int still = dry ? 0 : restore_auto();
            cooldown = time(NULL) + COOLDOWN;
            if (cpu_bad >= 3) logmsg("cannot read the CPU temperature -> fans back to SMC%s", still ? " (some still read as manual)" : "");
            else              logmsg("CPU %.1f°C -> fans back to SMC%s", cpu_ema, still ? " (some still read as manual)" : "");
            wait_tick(cfg.interval);
            continue;
        }
        double cv = curve_value(&cfg, t);
        double gp = guard_pct(&cfg, cpu_ema);

        /* Each fan's level moves only after the change has been seen on several checks in a row
         * (confirm_up to go up, confirm_down to go down), then ramps: up by 2 % of the range per
         * second, down by 0.5 %. Differences under 100 rpm are ignored. */
        int started = 0;
        for (int i = 0; i < n; i++) {
            double range = rmax[i] - rmin[i];
            double want = curve_rpm(&cfg, cv, rmin[i], rmax[i]);
            if (gp >= 0) {
                double g = rmin[i] + gp / 100.0 * range;
                if (g > want) want = g;
            }
            if (want > lvl[i] + 100) {
                fall_n[i] = 0;
                if (moving[i] < 0) moving[i] = 0;
                if (moving[i] == 0 && ++rise_n[i] >= cfg.confirm_up) { moving[i] = 1; started = 1; }
                if (moving[i] > 0) lvl[i] = fmin(want, lvl[i] + RAMP_UP * range * dt);
            } else if (want < lvl[i] - 100) {
                rise_n[i] = 0;
                if (moving[i] > 0) moving[i] = 0;
                if (moving[i] == 0 && ++fall_n[i] >= cfg.confirm_down) { moving[i] = -1; started = -1; }
                if (moving[i] < 0) lvl[i] = fmax(want, lvl[i] - RAMP_DOWN * range * dt);
            } else {
                rise_n[i] = fall_n[i] = moving[i] = 0;
            }
        }
        if (started) {
            double want0 = curve_rpm(&cfg, cv, rmin[0], rmax[0]);
            const char *why = "curve";
            if (gp >= 0 && rmin[0] + gp / 100.0 * (rmax[0] - rmin[0]) > want0) {
                want0 = rmin[0] + gp / 100.0 * (rmax[0] - rmin[0]);
                why = "cpu guard";
            }
            logmsg("PMU %.1f°C  CPU %.1f°C -> fans %s towards %.0f rpm (%s)", t, cpu_ema,
                   started > 0 ? "up" : "down", want0, why);
        }

        int bad = 0, wrote = 0;
        int can_write = time(NULL) - last_write >= 2;  /* whole batches, at least 2 s apart */
        int verify = k % TG_EVERY == 0;
        char shown[200] = "";
        size_t len = 0;
        for (int i = 0; i < n; i++) {
            if (len < sizeof shown)
                len += snprintf(shown + len, sizeof shown - len, " fan%d %.0f%s", i, lvl[i],
                                moving[i] > 0 ? "+" : moving[i] < 0 ? "-" : "");
            if (dry || !can_write) continue;
            /* Write only when the level moved by more than 100 rpm, or macOS took the fan back
             * (mode 0 or 3), or someone else changed the target. Constant rewriting is throttled. */
            int need = fset[i] < 0 || fabs(lvl[i] - fset[i]) > 100;
            if (!need) { int m = fan_mode(i); need = m >= 0 && m != 1; }
            if (!need && verify) {
                float tg;
                if (fan_float(i, "Tg", &tg) == 0 && fabsf(tg - fset[i]) > 1) need = 1;
            }
            if (!need) continue;
            if (set_fan(i, (float)lvl[i]) != 0) { fset[i] = -1; bad++; continue; }
            fset[i] = (float)lvl[i];
            wrote = 1;
        }
        if (wrote) last_write = time(NULL);
        /* A Mac that never lets fanctl take a fan over: stop trying for a while. */
        take_fail = bad == n ? take_fail + 1 : 0;
        if (take_fail >= 3 && !dry) {
            manual = fans_ours = 0;
            up_n = down_n = take_fail = 0;
            restore_auto();
            cooldown = time(NULL) + 600;
            logmsg("cannot take over the fans on this Mac -> left to macOS, next try in 10 minutes");
            wait_tick(cfg.interval);
            continue;
        }
        if (dry && verbose) {
            printf("[dry] PMU %.1f°C  CPU %.1f°C (smoothed) ->%s rpm", t, cpu_ema, shown);
            if (down_n > 0) printf("  (hand-back check %d of %d)", down_n, cfg.confirm_down);
            printf("\n");
        }
        if (bad && time(NULL) - last_fail_log >= 60) {      /* at most one failure line a minute */
            logmsg("PMU %.1f°C: writing failed for %d fan%s", t, bad, bad == 1 ? "" : "s");
            last_fail_log = time(NULL);
        }
        if (verbose && !dry && k % 10 == 0)
            logmsg("PMU %.1f°C  CPU %.1f°C (smoothed) ->%s rpm", t, cpu_ema, shown);
        k++;
        wait_tick(cfg.interval);
    }

    if (!dry) {
        int still = restore_auto();
        logmsg("signal %d: %s, exiting", stop_sig, still ? "could not hand every fan back to SMC" : "fans back to SMC");
    } else {
        logmsg("signal %d, exiting", stop_sig);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  fanctl status\n"
        "  fanctl start | stop\n"
        "  fanctl autostart [on|off]\n"
        "  fanctl preset [NAME]\n"
        "  fanctl reload\n"
        "  fanctl temp\n"
        "  fanctl set <rpm> | fanctl set <fan#> <rpm>\n"
        "  fanctl auto\n"
        "  fanctl check [--config FILE]\n"
        "  fanctl run [--config FILE] [--dry-run] [--verbose]\n"
        "  fanctl version\n");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "status"))  return cmd_status();
    if (!strcmp(cmd, "start"))   return cmd_start();
    if (!strcmp(cmd, "stop"))    return cmd_stop();
    if (!strcmp(cmd, "autostart")) return cmd_autostart(argc - 2, argv + 2);
    if (!strcmp(cmd, "preset"))  return cmd_preset(argc - 2, argv + 2);
    if (!strcmp(cmd, "reload"))  return cmd_reload();
    if (!strcmp(cmd, "temp"))    return cmd_temp();
    if (!strcmp(cmd, "set"))     return cmd_set(argc - 2, argv + 2);
    if (!strcmp(cmd, "auto"))    return cmd_auto();
    if (!strcmp(cmd, "check"))   return cmd_check(argc - 2, argv + 2);
    if (!strcmp(cmd, "run"))     return cmd_run(argc - 2, argv + 2);
    if (!strcmp(cmd, "version")) { printf("fanctl %s\n", FANCTL_VERSION); return 0; }
    usage();
    return 2;
}
