// SPDX-License-Identifier: GPL-2.0-only
/*
 * sensorpanel — RV32 NOMMU FLAT renderer that mmap()'s /dev/fb0 (RGB565)
 * and draws live BME680 + BMI270 readings + a temperature sparkline at
 * ~3 Hz. Built for the WHY2025 badge running native Linux 6.18.35 LTS on
 * the ESP32-P4 HP core. The "look, it's Linux!" demo for FOSDEM 2027.
 *
 * Design constraints (all learned the hard way; see CLAUDE.md / memory):
 * - Raw write(2)/read(2) only: uClibc stdio FILE* drags in pthread/locale
 *   globals whose R_RISCV_32 relocs elf2flt drops on the floor → SIGSEGV
 *   at low addresses on the first FILE-touching call.
 * - No u64/u64 division: the helper hangs forever on the buildroot gcc
 *   13.x + uClibc combo (observed on 13.3.0). Kernel-side or userspace,
 *   same trap.
 * - DON'T mmap /dev/fb0. drm_fbdev_dma's fb_mmap path doesn't work on
 *   this NOMMU build (returns -EINVAL or -ENOMEM); witnessed via
 *   "sensorpanel: mmap failed" in /tmp/sp.log. Use the same pattern
 *   fbdoom uses: anonymous-mmap an off-screen shadow (goes through the
 *   nommu_userspace_pool reservation in DT), draw into it, then a
 *   single pwrite() per frame blits to /dev/fb0. drm_fbdev_dma's
 *   write_iter handler does the dirty notification → pipe_update
 *   rotates + scans out.
 * - Match IIO devices by name, not by iio:device0/1 ordering — probe
 *   order depends on which device wins the i2c-gpio race per boot.
 *
 * Built from the buildroot uclibc + elf2flt toolchain via overlay-src/Makefile.
 */

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define PANEL_W 720
#define PANEL_H 720

/* RGB565 colour helpers. */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

#define COL_BG       rgb565(  6,   8,  20)   /* near-black, slight blue */
#define COL_FG       rgb565(220, 230, 240)   /* off-white */
#define COL_DIM      rgb565( 90, 100, 130)   /* footer/labels */
#define COL_TEMP     rgb565(255, 130,  60)   /* orange */
#define COL_HUM      rgb565( 80, 170, 255)   /* blue */
#define COL_PRES     rgb565(140, 220, 140)   /* green */
#define COL_AX       rgb565(255,  90,  90)   /* red */
#define COL_AY       rgb565( 90, 255,  90)   /* green */
#define COL_AZ       rgb565( 90, 130, 255)   /* blue */
#define COL_SPARK    rgb565(255, 200,  90)
#define COL_SPARK_BG rgb565( 30,  35,  50)
#define COL_TITLE    rgb565(255, 220, 100)

/*
 * 8x8 ASCII font, 0x20..0x7F.
 * Source: classic public-domain "8x8 fixed" / Damian Yerrick's pd font set
 * (released to public domain). Trimmed inline because shipping a ttf
 * + freetype renderer would 5x our binary size.
 */
static const uint8_t font8x8[96][8] = {
    {0,0,0,0,0,0,0,0},                                      /* 0x20 ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},              /* '!' */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},              /* '"' */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},              /* '#' */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},              /* '$' */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},              /* '%' */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},              /* '&' */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},              /* '\'' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},              /* '(' */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},              /* ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},              /* '*' */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},              /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},              /* ',' */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},              /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},              /* '.' */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},              /* '/' */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},              /* '0' */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},              /* '1' */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},              /* '2' */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},              /* '3' */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},              /* '4' */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},              /* '5' */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},              /* '6' */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},              /* '7' */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},              /* '8' */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},              /* '9' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},              /* ':' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},              /* ';' */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},              /* '<' */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},              /* '=' */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},              /* '>' */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},              /* '?' */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},              /* '@' */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},              /* 'A' */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},              /* 'B' */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},              /* 'C' */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},              /* 'D' */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},              /* 'E' */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},              /* 'F' */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},              /* 'G' */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},              /* 'H' */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},              /* 'I' */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},              /* 'J' */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},              /* 'K' */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},              /* 'L' */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},              /* 'M' */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},              /* 'N' */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},              /* 'O' */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},              /* 'P' */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},              /* 'Q' */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},              /* 'R' */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},              /* 'S' */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},              /* 'T' */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},              /* 'U' */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},              /* 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},              /* 'W' */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},              /* 'X' */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},              /* 'Y' */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},              /* 'Z' */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},              /* '[' */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},              /* '\\' */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},              /* ']' */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},              /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},              /* '_' */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},              /* '`' */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},              /* 'a' */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},              /* 'b' */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},              /* 'c' */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},              /* 'd' */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},              /* 'e' */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},              /* 'f' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},              /* 'g' */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},              /* 'h' */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},              /* 'i' */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},              /* 'j' */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},              /* 'k' */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},              /* 'l' */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},              /* 'm' */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},              /* 'n' */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},              /* 'o' */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},              /* 'p' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},              /* 'q' */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},              /* 'r' */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},              /* 's' */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},              /* 't' */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},              /* 'u' */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},              /* 'v' */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},              /* 'w' */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},              /* 'x' */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},              /* 'y' */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},              /* 'z' */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},              /* '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},              /* '|' */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},              /* '}' */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},              /* '~' */
    {0,0,0,0,0,0,0,0},                                      /* DEL */
};

static uint16_t *fb;            /* anonymous-mmap'd shadow buffer */
static int      fb_stride_px;   /* line_length / 2 */
static int      fb_w, fb_h;
static int      fb_fd = -1;     /* open fd to /dev/fb0 */
static size_t   fb_bytes;       /* line_length * yres */

static void px(int x, int y, uint16_t c)
{
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h) return;
    fb[y * fb_stride_px + x] = c;
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t c)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > fb_w) w = fb_w - x0;
    if (y0 + h > fb_h) h = fb_h - y0;
    if (w <= 0 || h <= 0) return;
    for (int y = y0; y < y0 + h; y++) {
        uint16_t *row = &fb[y * fb_stride_px + x0];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

/*
 * Draw one glyph at integer scale s. Each font row is 8 bits, LSB on the
 * left (matches the public-domain font layout above).
 */
static void draw_char(int x, int y, char ch, int s, uint16_t c)
{
    if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7F) ch = '?';
    const uint8_t *g = font8x8[(unsigned char)ch - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                /* Each glyph pixel becomes an s×s solid block. */
                fill_rect(x + col * s, y + row * s, s, s, c);
            }
        }
    }
}

static int draw_str(int x, int y, const char *s, int scale, uint16_t c)
{
    int x0 = x;
    while (*s) {
        draw_char(x, y, *s, scale, c);
        x += 8 * scale;
        s++;
    }
    return x - x0;
}

/* ---- I/O helpers (no FILE*) ---- */

/*
 * Read a sysfs file into the supplied buffer. Returns bytes read or -1.
 * Trims the trailing newline so the caller can treat it as a C string.
 */
static int slurp(const char *path, char *buf, int max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, max - 1);
    close(fd);
    if (n < 0) return -1;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) n--;
    buf[n] = 0;
    return n;
}

/* Parse a possibly-signed integer from str, stop at first non-digit. */
static int parse_int(const char *s, int *out)
{
    int sign = 1, v = 0, any = 0;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++; any = 1;
    }
    if (!any) return -1;
    *out = sign * v;
    return 0;
}

/*
 * Parse "INT.FRAC" into integer*scale (e.g. 3 decimal digits → milli-).
 * Returns 0 on success, -1 on parse failure. Output is signed micro/milli
 * units depending on scale_digits. Truncates extra fractional digits.
 */
static int parse_fixed(const char *s, int scale_digits, int *out)
{
    int sign = 1, ip = 0, fp = 0, fdigits = 0;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    int any = 0;
    while (*s >= '0' && *s <= '9') {
        ip = ip * 10 + (*s - '0');
        s++; any = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && fdigits < scale_digits) {
            fp = fp * 10 + (*s - '0');
            s++; fdigits++;
        }
        while (*s >= '0' && *s <= '9') s++;     /* skip extra precision */
    }
    while (fdigits < scale_digits) { fp *= 10; fdigits++; }
    if (!any) return -1;
    int scale = 1;
    for (int i = 0; i < scale_digits; i++) scale *= 10;
    *out = sign * (ip * scale + fp);
    return 0;
}

/* ---- Number formatting ---- */

/*
 * Render a fixed-point integer into buf as decimal with `decimals`
 * places (e.g. value=3198, scale=2 → "31.98"). value is signed.
 * Returns number of characters written (excluding the NUL).
 */
static int fmt_fixed(char *buf, int max, int value, int decimals, int width)
{
    int neg = 0;
    if (value < 0) { neg = 1; value = -value; }
    int scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int ip = value / scale;
    int fp = value % scale;
    /* Build into a temporary then space-pad-left for alignment. */
    char tmp[32]; int n = 0;
    /* fractional part (LSD-first) */
    int fpd = decimals;
    while (fpd--) { tmp[n++] = '0' + (fp % 10); fp /= 10; }
    if (decimals > 0) tmp[n++] = '.';
    /* integer part (LSD-first) */
    if (ip == 0) tmp[n++] = '0';
    while (ip > 0) { tmp[n++] = '0' + (ip % 10); ip /= 10; }
    if (neg) tmp[n++] = '-';
    /* reverse + pad */
    int pad = (width > n) ? (width - n) : 0;
    int out = 0;
    while (pad-- && out < max - 1) buf[out++] = ' ';
    while (n-- && out < max - 1) buf[out++] = tmp[n];
    buf[out] = 0;
    return out;
}

/* ---- IIO device discovery ---- */

#define BME680_PATH_LEN 64
static char bme680_dir[BME680_PATH_LEN];   /* e.g. /sys/bus/iio/devices/iio:device0 */
static char bmi270_dir[BME680_PATH_LEN];

static int find_iio_devs(void)
{
    bme680_dir[0] = bmi270_dir[0] = 0;
    char namepath[BME680_PATH_LEN + 8];
    char namebuf[32];
    /* iio:device0..7 is the possible range; ten is overkill but cheap. */
    for (int i = 0; i < 10; i++) {
        int n = 0;
        const char *p = "/sys/bus/iio/devices/iio:device";
        while (*p && n < BME680_PATH_LEN - 1) bme680_dir[n++] = *p++;
        bme680_dir[n++] = '0' + i;
        bme680_dir[n] = 0;
        /* probe by reading $dir/name */
        for (int j = 0; bme680_dir[j]; j++) namepath[j] = bme680_dir[j];
        int dl = 0; while (bme680_dir[dl]) dl++;
        namepath[dl++] = '/';
        const char *nm = "name";
        while (*nm) namepath[dl++] = *nm++;
        namepath[dl] = 0;
        if (slurp(namepath, namebuf, sizeof namebuf) < 0) {
            bme680_dir[0] = 0;
            continue;
        }
        if (strcmp(namebuf, "bme680") == 0) {
            /* keep current bme680_dir */
        } else if (strcmp(namebuf, "bmi270") == 0) {
            int k = 0; while (bme680_dir[k]) { bmi270_dir[k] = bme680_dir[k]; k++; }
            bmi270_dir[k] = 0;
            bme680_dir[0] = 0;     /* clear so the next match for bme680 wins */
        } else {
            bme680_dir[0] = 0;
        }
    }
    /* Re-scan once to find bme680 even after we cleared its slot. */
    if (bme680_dir[0] == 0) {
        for (int i = 0; i < 10; i++) {
            int n = 0;
            const char *p = "/sys/bus/iio/devices/iio:device";
            while (*p && n < BME680_PATH_LEN - 1) bme680_dir[n++] = *p++;
            bme680_dir[n++] = '0' + i;
            bme680_dir[n] = 0;
            for (int j = 0; bme680_dir[j]; j++) namepath[j] = bme680_dir[j];
            int dl = 0; while (bme680_dir[dl]) dl++;
            namepath[dl++] = '/';
            const char *nm = "name";
            while (*nm) namepath[dl++] = *nm++;
            namepath[dl] = 0;
            if (slurp(namepath, namebuf, sizeof namebuf) < 0) { bme680_dir[0] = 0; continue; }
            if (strcmp(namebuf, "bme680") == 0) break;
            bme680_dir[0] = 0;
        }
    }
    return (bme680_dir[0] != 0) || (bmi270_dir[0] != 0) ? 0 : -1;
}

static int read_attr(const char *dir, const char *attr, char *buf, int max)
{
    char path[96];
    int n = 0;
    while (*dir && n < (int)sizeof(path) - 1) path[n++] = *dir++;
    if (n < (int)sizeof(path) - 1) path[n++] = '/';
    while (*attr && n < (int)sizeof(path) - 1) path[n++] = *attr++;
    path[n] = 0;
    return slurp(path, buf, max);
}

/* ---- App state ---- */

#define SPARK_W   600       /* pixels of sparkline area */
#define SPARK_H    80
#define SPARK_N    20       /* sample count (downsampled to SPARK_W) */
static int temp_hist[SPARK_N];   /* in centi-degC */
static int temp_hist_n;
static int temp_hist_min, temp_hist_max;

static void hist_push(int v)
{
    if (temp_hist_n < SPARK_N) {
        temp_hist[temp_hist_n++] = v;
    } else {
        for (int i = 1; i < SPARK_N; i++) temp_hist[i-1] = temp_hist[i];
        temp_hist[SPARK_N - 1] = v;
    }
    /* recompute extents */
    temp_hist_min = temp_hist_max = temp_hist[0];
    for (int i = 1; i < temp_hist_n; i++) {
        if (temp_hist[i] < temp_hist_min) temp_hist_min = temp_hist[i];
        if (temp_hist[i] > temp_hist_max) temp_hist_max = temp_hist[i];
    }
    /* widen if values clustered to keep the sparkline non-degenerate */
    if (temp_hist_max - temp_hist_min < 40) {
        int mid = (temp_hist_max + temp_hist_min) / 2;
        temp_hist_min = mid - 20;
        temp_hist_max = mid + 20;
    }
}

static void draw_sparkline(int x0, int y0, int w, int h)
{
    fill_rect(x0, y0, w, h, COL_SPARK_BG);
    /* axis tick on left */
    fill_rect(x0, y0, 2, h, COL_DIM);
    if (temp_hist_n < 2) return;
    int range = temp_hist_max - temp_hist_min;
    if (range < 1) range = 1;
    int prev_x = -1, prev_y = -1;
    for (int i = 0; i < temp_hist_n; i++) {
        int x = x0 + (i * (w - 4)) / (SPARK_N - 1) + 2;
        /* y: top of plot = max, bottom = min */
        int y = y0 + h - 4 - ((temp_hist[i] - temp_hist_min) * (h - 8)) / range;
        if (prev_x >= 0) {
            /* short bresenham line */
            int dx = x - prev_x, dy = y - prev_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int steps = adx > ady ? adx : ady;
            if (steps < 1) steps = 1;
            for (int s = 0; s <= steps; s++) {
                int xi = prev_x + (dx * s) / steps;
                int yi = prev_y + (dy * s) / steps;
                fill_rect(xi - 1, yi - 1, 3, 3, COL_SPARK);
            }
        }
        prev_x = x; prev_y = y;
    }
}

/* ---- Main render ---- */

/*
 * The panel is 720×720 RGB565. The DRM driver does the 90° CCW rotation
 * + 16-px panel vshift internally in pipe_update, so userspace draws into
 * a regular upright framebuffer; what we write at (x, y) shows up as
 * what the user sees at that (x, y) on the physical panel — confirmed
 * empirically during DSI bring-up.
 */
static void render_frame(int frame, int have_bme, int have_bmi,
                         int t_centi, int rh_centi, int p_pa_div10,
                         int ax_mg, int ay_mg, int az_mg)
{
    char buf[64];

    /* Fresh background each frame keeps stale glyphs from accumulating
     * when a value's printed width changes (e.g. -9 → -10). */
    fill_rect(0, 0, fb_w, fb_h, COL_BG);

    /* Title strip at top. */
    fill_rect(0, 0, fb_w, 56, rgb565(20, 24, 40));
    draw_str(20, 18, "WHY2025  *  ESP32-P4  *  Linux 6.18", 3, COL_TITLE);

    int y = 90;

    /* --- Environment: temp / humidity / pressure --- */
    if (have_bme) {
        /* Temp: 31980 mC -> 31.98 (2 decimals). */
        int n = fmt_fixed(buf, sizeof buf, t_centi, 2, 5);
        (void)n;
        draw_str(40, y, buf, 6, COL_TEMP);
        draw_str(40 + 5*8*6 + 40, y + 22, "C", 4, COL_TEMP);
        draw_str(40 + 5*8*6 + 40 - 24, y - 4, ".", 2, COL_TEMP);    /* degree dot */
        draw_str(360, y + 50, "TEMPERATURE", 2, COL_DIM);
        y += 110;

        /* Humidity: parsed * 1000 → centi%. Render 2 decimals. */
        fmt_fixed(buf, sizeof buf, rh_centi, 2, 5);
        draw_str(40, y, buf, 6, COL_HUM);
        draw_str(40 + 5*8*6 + 40, y + 22, "%", 4, COL_HUM);
        draw_str(360, y + 50, "REL. HUMIDITY", 2, COL_DIM);
        y += 110;

        /* Pressure: deci-kPa as parsed (e.g. 1843 → "184.3"). */
        fmt_fixed(buf, sizeof buf, p_pa_div10, 1, 5);
        draw_str(40, y, buf, 6, COL_PRES);
        draw_str(40 + 5*8*6 + 40, y + 22, "kPa", 3, COL_PRES);
        draw_str(360, y + 50, "PRESSURE", 2, COL_DIM);
        y += 110;
    } else {
        draw_str(40, y, "BME680 N/A", 4, COL_DIM);
        y += 110;
    }

    /* --- Accel --- */
    if (have_bmi) {
        const char *labels[3] = { "ax", "ay", "az" };
        int vals[3] = { ax_mg, ay_mg, az_mg };
        uint16_t cols[3] = { COL_AX, COL_AY, COL_AZ };
        int xstart = 40;
        int xstep = (fb_w - 80) / 3;
        for (int i = 0; i < 3; i++) {
            int x = xstart + i * xstep;
            draw_str(x, y + 4, labels[i], 3, COL_DIM);
            /* mg as +0.123 (3 decimals). */
            fmt_fixed(buf, sizeof buf, vals[i], 3, 6);
            draw_str(x, y + 36, buf, 4, cols[i]);
            draw_str(x + 6*8*4 + 6, y + 36 + 8, "g", 2, cols[i]);
        }
        y += 90;
    } else {
        draw_str(40, y, "BMI270 N/A", 4, COL_DIM);
        y += 90;
    }

    /* --- Sparkline --- */
    int sx = (fb_w - SPARK_W) / 2;
    draw_str(sx, y, "Temperature, last 20 samples", 2, COL_DIM);
    draw_sparkline(sx, y + 22, SPARK_W, SPARK_H);
    y += SPARK_H + 36;

    /* --- Footer / heartbeat --- */
    fmt_fixed(buf, sizeof buf, frame, 0, 0);
    char foot[80];
    int n = 0;
    const char *f1 = "Linux 6.18.35 - RV32IMA NOMMU - frame #";
    while (*f1) foot[n++] = *f1++;
    int k = 0; while (buf[k]) foot[n++] = buf[k++];
    foot[n] = 0;
    draw_str(20, fb_h - 36, foot, 2, COL_DIM);
}

int main(void)
{
    write(1, "sensorpanel: start\n", 19);

    /* --- Open framebuffer (no mmap on /dev/fb0 — see header comment) --- */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        write(2, "sensorpanel: open /dev/fb0 failed\n", 34);
        return 1;
    }
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        write(2, "sensorpanel: FBIOGET_*SCREENINFO failed\n", 40);
        return 1;
    }
    fb_w = vi.xres;
    fb_h = vi.yres;
    fb_stride_px = fi.line_length / 2;
    if (vi.bits_per_pixel != 16) {
        write(2, "sensorpanel: not RGB565\n", 24);
        return 1;
    }

    /* Allocate the shadow as an anonymous mmap. On this NOMMU kernel that
     * routes through nommu_userspace_pool (10 MB DT-reserved at 0x49600000),
     * giving us one contiguous block. Drawing into the shadow then a single
     * pwrite() per frame blits the whole panel atomically. */
    fb_bytes = (size_t)fi.line_length * vi.yres;
    fb = (uint16_t *)mmap(NULL, fb_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fb == (uint16_t *)-1) {
        write(2, "sensorpanel: shadow mmap failed\n", 32);
        return 1;
    }
    write(1, "sensorpanel: fb + shadow ready\n", 31);

    /* --- Discover IIO devices, with retry --- */
    /* BMI270 enumerates a couple seconds *after* init starts because of
     * S05bmi270 (post-rootfs-mount sysfs rebind). Retry every 250 ms for
     * up to 5 s so we don't have to coordinate startup ordering. */
    {
        int attempts = 20;
        while (attempts--) {
            find_iio_devs();
            if (bme680_dir[0] && bmi270_dir[0]) break;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 250 * 1000 * 1000 };
            nanosleep(&ts, 0);
        }
    }
    write(1, "sensorpanel: iio scan: bme=", 27);
    write(1, bme680_dir[0] ? "y" : "n", 1);
    write(1, " bmi=", 5);
    write(1, bmi270_dir[0] ? "y" : "n", 1);
    write(1, "\n", 1);

    /* --- Read BMI270 scale ONCE (constant per chip range). --- */
    /* in_accel_scale is m/s² per LSB, e.g. "0.000598624" at ±2 g. */
    int accel_scale_micro = 598;     /* 0.000598624 → 598 µ(m/s²)/LSB */
    if (bmi270_dir[0]) {
        char tmp[64];
        if (read_attr(bmi270_dir, "in_accel_scale", tmp, sizeof tmp) > 0) {
            int v;
            if (parse_fixed(tmp, 9, &v) == 0) {
                /* v is now scale × 1e9. We want micro, so / 1000. */
                accel_scale_micro = v / 1000;
                if (accel_scale_micro < 1) accel_scale_micro = 1;
            }
        }
    }

    int frame = 0;
    for (;;) {
        char buf[64];

        int t_centi = 0, rh_centi = 0, p_milli = 0;
        int have_bme = 0;
        if (bme680_dir[0]) {
            if (read_attr(bme680_dir, "in_temp_input", buf, sizeof buf) > 0) {
                int v;
                if (parse_int(buf, &v) == 0) {
                    /* milli-°C → centi-°C: divide by 10. */
                    t_centi = v / 10;
                    have_bme = 1;
                }
            }
            if (read_attr(bme680_dir, "in_humidityrelative_input", buf, sizeof buf) > 0) {
                int v;
                /* Driver emits "24.023000000". 2 decimals → centi-%. */
                if (parse_fixed(buf, 2, &v) == 0) {
                    rh_centi = v;
                }
            }
            if (read_attr(bme680_dir, "in_pressure_input", buf, sizeof buf) > 0) {
                int v;
                /* Driver emits kPa with 9-digit fraction; we keep 1 decimal
                 * place so render_frame's 1-decimal formatter prints "100.3"
                 * for typical sea-level. (This BME680 family chip on the
                 * WHY2025 reads ~225-250 kPa instead of ~100 kPa — a known
                 * calibration coefficient mismatch between BME680 vs
                 * BME688/690, not a unit bug; see HARDWARE.md.) */
                if (parse_fixed(buf, 1, &v) == 0) {
                    p_milli = v;            /* deci-kPa */
                }
            }
        }

        int ax_raw = 0, ay_raw = 0, az_raw = 0;
        int have_bmi = 0;
        if (bmi270_dir[0]) {
            int v;
            if (read_attr(bmi270_dir, "in_accel_x_raw", buf, sizeof buf) > 0 &&
                parse_int(buf, &v) == 0) { ax_raw = v; have_bmi = 1; }
            if (read_attr(bmi270_dir, "in_accel_y_raw", buf, sizeof buf) > 0 &&
                parse_int(buf, &v) == 0) { ay_raw = v; }
            if (read_attr(bmi270_dir, "in_accel_z_raw", buf, sizeof buf) > 0 &&
                parse_int(buf, &v) == 0) { az_raw = v; }
        }

        /* Convert raw → milli-g.
         *   m_per_s2 = raw * accel_scale_micro * 1e-6
         *   g       = m_per_s2 / 9.80665
         *   mg      = g * 1000 = raw * accel_scale_micro / 9806.65
         *   ≈ raw * accel_scale_micro / 9807   (one in 0.04 % error)
         * All math fits in 32-bit signed: raw in [-32768,32767],
         * scale_micro typically 598, product ~2e7. */
        int ax_mg = (ax_raw * accel_scale_micro) / 9807;
        int ay_mg = (ay_raw * accel_scale_micro) / 9807;
        int az_mg = (az_raw * accel_scale_micro) / 9807;

        if (have_bme) hist_push(t_centi);

        render_frame(frame, have_bme, have_bmi,
                     t_centi, rh_centi, p_milli,
                     ax_mg, ay_mg, az_mg);

        /* Blit shadow → /dev/fb0 in one shot. drm_fbdev_dma's write
         * helper marks the affected pages dirty; pipe_update then
         * rotates + scans out. */
        ssize_t r = pwrite(fb_fd, fb, fb_bytes, 0);
        if (r != (ssize_t)fb_bytes) {
            write(2, "sensorpanel: short pwrite\n", 26);
        }

        if ((frame & 0x1f) == 0) {
            /* Heartbeat every ~10 s so we can tell the loop is alive
             * via /tmp/sp.log without flooding it. */
            write(1, "sensorpanel: tick\n", 18);
        }

        frame++;

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 333 * 1000 * 1000 };
        nanosleep(&ts, 0);
    }

    return 0;       /* unreached */
}
