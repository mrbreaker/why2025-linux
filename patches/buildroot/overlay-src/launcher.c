// SPDX-License-Identifier: MIT
/*
 * launcher — keypad-driven menu on the DSI panel.
 *
 * Run it from the badge console (tty1): draws a menu straight onto
 * /dev/fb0 (same shadow-buffer + one-pwrite-per-frame pattern as
 * sensorpanel — no fbdev mmap on NOMMU) and takes the keypad
 * exclusively via EVIOCGRAB (same as sleepd's sleep mode), so arrow
 * keys don't leak to the shell underneath. Entries:
 *
 *   Sensor panel   spawns sensorpanel (draws, needs no input); the
 *                  grab stays here, any key kills it and returns
 *   DOOM           only listed when /usr/bin/fbdoom exists; the grab
 *                  is RELEASED so fbdoom gets tty input, rejoined on
 *                  exit (fbdoom's own exit key is Backspace). The
 *                  menu's 1 MB shadow is munmap'd for the duration:
 *                  fbdoom's -mb 4 zone comes from the same pool, and
 *                  an explicit -mb N is single-shot (fbdoom sets
 *                  min_ram = default_ram — no auto-shrink).
 *   Wi-Fi status   operstate + MAC from sysfs, any key returns
 *   BLE advertise  toggles ble_adv (broadcasts the hostname so other
 *                  badges' ble_scan can see it); left running on quit
 *   Backlight      cycles display brightness 76 -> 150 -> 255 -> 25
 *   Quit / Esc     ungrab + exit (fbcon repaints on next console
 *                  output; hit Enter at the shell)
 *
 * While the menu is up the grab also starves sleepd of events, so
 * Fn+Esc display sleep doesn't fire — quit the menu first.
 *
 * Children are spawned with vfork+execv (NOMMU: no fork; the parent
 * is suspended until the child execs — that's fine, the child execs
 * immediately). One child at a time, reaped with waitpid.
 *
 * FLAT/NOMMU constraints (see Makefile): -fPIC, no stdio, raw
 * write(2)/read(2) + sysfs only.
 */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/fb.h>

#define EVDEV       "/dev/input/event0"
#define BRIGHTNESS  "/sys/class/backlight/backlight/brightness"

#define EV_KEY      1
#define KEY_ESC     1
#define KEY_ENTER   28
#define KEY_UP      103
#define KEY_DOWN    108
#define EVIOCGRAB   0x40045590	/* _IOW('E', 0x90, int) */

/* evdev record as emitted by a 32-bit kernel (same as sleepd.c). */
struct ev {
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

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

static uint16_t *fb;		/* anonymous-mmap'd shadow buffer */
static int       fb_stride_px;
static int       fb_w, fb_h;
static int       fb_fd = -1;
static size_t    fb_bytes;
static int       ev_fd = -1;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

#define COL_BG     rgb565(8, 10, 18)
#define COL_TITLE  rgb565(120, 200, 255)
#define COL_ITEM   rgb565(200, 200, 210)
#define COL_SEL    rgb565(255, 255, 255)
#define COL_SELBG  rgb565(40, 70, 120)
#define COL_DIM    rgb565(110, 110, 125)
#define COL_OK     rgb565(120, 230, 120)

static void say(const char *s) { write(2, s, strlen(s)); }

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

static void draw_char(int x, int y, char ch, int s, uint16_t c)
{
	if (ch < 0x20 || ch > 0x7f) ch = '?';
	const uint8_t *g = font8x8[(unsigned char)ch - 0x20];
	for (int r = 0; r < 8; r++)
		for (int b = 0; b < 8; b++)
			if (g[r] & (1 << b))
				fill_rect(x + b * s, y + r * s, s, s, c);
}

static int draw_str(int x, int y, const char *s, int scale, uint16_t c)
{
	while (*s) {
		draw_char(x, y, *s, scale, c);
		x += 8 * scale;
		s++;
	}
	return x;
}

static void blit(void)
{
	pwrite(fb_fd, fb, fb_bytes, 0);
}

static int read_sysfs_str(const char *path, char *buf, int max)
{
	int fd = open(path, O_RDONLY);
	int n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, max - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		n--;
	buf[n] = 0;
	return n;
}

static void write_sysfs_str(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		return;
	write(fd, v, strlen(v));
	close(fd);
}

/* One child at a time; vfork suspends us until the child execs. */
static int spawn(char *const argv[])
{
	int pid = vfork();

	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	return pid;
}

/* Block until a key PRESS arrives; returns its code. */
static int wait_key(void)
{
	struct ev e;

	for (;;) {
		int n = read(ev_fd, &e, sizeof(e));
		if (n != sizeof(e)) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			return -1;	/* device went away */
		}
		if (e.type == EV_KEY && e.value == 1)
			return e.code;
	}
}

/* ---- Screens ---- */

static int ble_pid = -1;

/*
 * ble_adv enables BT itself (bt_enable knob) and parks until killed;
 * first start takes a few seconds while hci0 comes up. Deliberately
 * NOT reaped on quit — advertising keeps running, same as `ble_adv &`.
 */
static void toggle_ble_adv(void)
{
	char *argv[] = { "/usr/bin/ble_adv", 0 };

	if (ble_pid > 0) {
		kill(ble_pid, SIGTERM);
		waitpid(ble_pid, 0, 0);
		ble_pid = -1;
		return;
	}
	ble_pid = spawn(argv);
}

static const int bl_steps[] = { 76, 150, 255, 25 };
static int bl_idx;

static void cycle_backlight(void)
{
	char buf[8];
	int v, n = 0;

	bl_idx = (bl_idx + 1) % 4;
	v = bl_steps[bl_idx];
	char tmp[8]; int m = 0;
	while (v) { tmp[m++] = (char)('0' + v % 10); v /= 10; }
	while (m--) buf[n++] = tmp[m];
	buf[n] = 0;
	write_sysfs_str(BRIGHTNESS, buf);
}

static void wifi_screen(void)
{
	char state[32], mac[32];

	fill_rect(0, 0, fb_w, fb_h, COL_BG);
	draw_str(40, 40, "Wi-Fi", 5, COL_TITLE);
	if (read_sysfs_str("/sys/class/net/wlan0/operstate",
			   state, sizeof state) < 0) {
		draw_str(40, 160, "wlan0 not present", 3, COL_DIM);
		draw_str(40, 220, "(esp-hosted still probing?)", 2, COL_DIM);
	} else {
		draw_str(40, 160, "state:", 3, COL_DIM);
		draw_str(240, 160, state,  3,
			 state[0] == 'u' ? COL_OK : COL_ITEM);
		if (read_sysfs_str("/sys/class/net/wlan0/address",
				   mac, sizeof mac) > 0) {
			draw_str(40, 220, "mac:", 3, COL_DIM);
			draw_str(240, 220, mac, 3, COL_ITEM);
		}
		draw_str(40, 300, "join: wifi-connect '<ssid>' '<psk>'",
			 2, COL_DIM);
		draw_str(40, 340, "persist: /mnt/sd/badge/wifi.conf",
			 2, COL_DIM);
	}
	draw_str(40, fb_h - 60, "any key returns", 2, COL_DIM);
	blit();
	wait_key();
}

static void run_sensorpanel(void)
{
	char *argv[] = { "/usr/bin/sensorpanel", 0 };
	int pid;

	/* sensorpanel draws but reads no input: keep the grab, any key
	 * tears it down and returns to the menu. */
	fill_rect(0, 0, fb_w, fb_h, COL_BG);
	blit();
	pid = spawn(argv);
	if (pid < 0)
		return;
	wait_key();
	kill(pid, SIGTERM);
	waitpid(pid, 0, 0);
}

static void run_doom(void)
{
	char *argv[] = { "/usr/bin/fbdoom",
			 "-iwad", "/usr/share/games/doom/doom1.wad",
			 "-mb", "4", 0 };
	int pid, status;

	/* fbdoom reads the tty: hand the keypad back while it runs.
	 * Its own exit key is Backspace. Also hand our 1 MB shadow back
	 * to the pool for the duration — fbdoom's contiguous -mb 4 zone
	 * comes from the same place, and an explicit -mb is single-shot
	 * (no auto-shrink). */
	fill_rect(0, 0, fb_w, fb_h, 0);
	blit();
	munmap(fb, fb_bytes);
	fb = NULL;
	ioctl(ev_fd, EVIOCGRAB, 0);
	pid = spawn(argv);
	if (pid >= 0)
		waitpid(pid, &status, 0);
	ioctl(ev_fd, EVIOCGRAB, 1);
	fb = (uint16_t *)mmap(NULL, fb_bytes, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (fb == (uint16_t *)-1) {
		/* Pool couldn't hand the shadow back — exit cleanly to
		 * the console rather than draw through a bad pointer. */
		say("launcher: shadow re-mmap failed after doom\n");
		ioctl(ev_fd, EVIOCGRAB, 0);
		close(ev_fd);
		close(fb_fd);
		_exit(1);
	}
}

/* ---- Menu ---- */

struct entry {
	const char *label;
	int id;
};
enum { E_SENSORS, E_DOOM, E_WIFI, E_BLEADV, E_BACKLIGHT, E_QUIT };

int main(void)
{
	struct entry entries[8];
	int n_entries = 0, sel = 0, have_doom;

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		say("launcher: open /dev/fb0 failed\n");
		return 1;
	}
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fi) < 0) {
		say("launcher: FBIOGET_*SCREENINFO failed\n");
		return 1;
	}
	fb_w = vi.xres;
	fb_h = vi.yres;
	fb_stride_px = fi.line_length / 2;
	if (vi.bits_per_pixel != 16) {
		say("launcher: not RGB565\n");
		return 1;
	}
	fb_bytes = (size_t)fi.line_length * vi.yres;
	fb = (uint16_t *)mmap(NULL, fb_bytes, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (fb == (uint16_t *)-1) {
		say("launcher: shadow mmap failed\n");
		return 1;
	}

	ev_fd = open(EVDEV, O_RDONLY);
	if (ev_fd < 0) {
		say("launcher: open " EVDEV " failed\n");
		return 1;
	}
	if (ioctl(ev_fd, EVIOCGRAB, 1) < 0)
		say("launcher: EVIOCGRAB failed (sleepd asleep?)\n");

	have_doom = (access("/usr/bin/fbdoom", X_OK) == 0);

	entries[n_entries++] = (struct entry){ "Sensor panel", E_SENSORS };
	if (have_doom)
		entries[n_entries++] = (struct entry){ "DOOM", E_DOOM };
	entries[n_entries++] = (struct entry){ "Wi-Fi status", E_WIFI };
	entries[n_entries++] = (struct entry){ "BLE advertise", E_BLEADV };
	entries[n_entries++] = (struct entry){ "Backlight", E_BACKLIGHT };
	entries[n_entries++] = (struct entry){ "Quit", E_QUIT };

	for (;;) {
		/* Redraw the whole menu every input event; also repairs
		 * anything fbcon painted over us in the meantime. */
		fill_rect(0, 0, fb_w, fb_h, COL_BG);
		fill_rect(0, 0, fb_w, 64, rgb565(20, 24, 40));
		draw_str(24, 20, "WHY2025", 4, COL_TITLE);
		draw_str(300, 28, "badge menu", 2, COL_DIM);

		int y = 120;
		for (int i = 0; i < n_entries; i++) {
			uint16_t c = (i == sel) ? COL_SEL : COL_ITEM;

			if (i == sel)
				fill_rect(24, y - 10, fb_w - 48, 52, COL_SELBG);
			int xe = draw_str(40, y, entries[i].label, 4, c);
			if (entries[i].id == E_BLEADV && ble_pid > 0)
				draw_str(xe + 24, y + 8, "[on]", 2, COL_OK);
			y += 70;
		}
		draw_str(24, fb_h - 60,
			 "arrows move - enter selects - esc quits", 2, COL_DIM);
		blit();

		int key = wait_key();
		if (key < 0)
			break;
		if (key == KEY_UP)
			sel = (sel + n_entries - 1) % n_entries;
		else if (key == KEY_DOWN)
			sel = (sel + 1) % n_entries;
		else if (key == KEY_ESC)
			break;
		else if (key == KEY_ENTER) {
			switch (entries[sel].id) {
			case E_SENSORS:   run_sensorpanel(); break;
			case E_DOOM:      run_doom(); break;
			case E_WIFI:      wifi_screen(); break;
			case E_BLEADV:    toggle_ble_adv(); break;
			case E_BACKLIGHT: cycle_backlight(); break;
			case E_QUIT:      goto out;
			}
		}
	}
out:
	ioctl(ev_fd, EVIOCGRAB, 0);
	fill_rect(0, 0, fb_w, fb_h, 0);
	blit();
	close(ev_fd);
	close(fb_fd);
	return 0;
}
