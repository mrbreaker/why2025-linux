// SPDX-License-Identifier: MIT
/* sleepd — Fn+Esc puts the badge display to sleep, any key wakes it.
 *
 * Sleep = fb blank (DPMS off) + backlight brightness 0 + EVIOCGRAB on
 * the keypad so keystrokes don't leak to the console while dark.
 * Wake = any key press: unblank, restore the saved brightness, ungrab.
 * The wake key's press is consumed by the grab; only its release
 * reaches the console, which produces no character.
 *
 * The right-side hardware button can't be used for this: it is wired
 * to the IP5306 KEY pin only (never reaches the P4), and the PMIC's
 * silicon handles it (single press = on, double press = hard power
 * off). The keypad is the only P4-visible input.
 *
 * FLAT/NOMMU constraints (see Makefile): no stdio, raw write(2) only.
 * This process must NEVER exit: an init respawn loop is sustained
 * fork+exec churn, which is this kernel's known wedge trigger
 * (docs/KNOWN-ISSUES.md) — all error paths retry internally instead.
 */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#define EVDEV        "/dev/input/event0"
#define BRIGHTNESS   "/sys/class/backlight/backlight/brightness"
#define FB_BLANK     "/sys/class/graphics/fb0/blank"

#define EV_KEY       1
#define KEY_ESC      1
#define KEY_FN       0x1d0	/* 464 */
#define EVIOCGRAB    0x40045590	/* _IOW('E', 0x90, int) */

#define BL_DEFAULT   76		/* C6 slave's BL_DISPLAY_INITIAL_DUTY */

/* evdev record as emitted by a 32-bit kernel: 2 x 32-bit timeval words,
 * then type/code/value. 16 bytes. */
struct ev {
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static void say(const char *s) { write(2, s, strlen(s)); }

static int read_sysfs_int(const char *path, int fallback)
{
	char buf[16];
	int fd, n, v = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return fallback;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return fallback;
	for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
		v = v * 10 + (buf[i] - '0');
	return v;
}

static void write_sysfs_int(const char *path, int v)
{
	char buf[16];
	int n = 0, fd;
	char tmp[16];

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return;
	if (v == 0) {
		write(fd, "0\n", 2);
	} else {
		int m = 0;
		while (v) { tmp[m++] = '0' + v % 10; v /= 10; }
		while (m--) buf[n++] = tmp[m];
		buf[n++] = '\n';
		write(fd, buf, n);
	}
	close(fd);
}

int main(void)
{
	int fd = -1, fn_down = 0, sleeping = 0, saved_bl = BL_DEFAULT;
	struct ev e;

	for (;;) {
		if (fd < 0) {
			fd = open(EVDEV, O_RDONLY);
			if (fd < 0) {
				/* keypad may not have probed yet — retry,
				 * never exit (respawn = fork churn). */
				sleep(2);
				continue;
			}
			say("sleepd: watching " EVDEV " (Fn+Esc sleeps)\n");
		}

		int n = read(fd, &e, sizeof(e));
		if (n != sizeof(e)) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			close(fd);	/* device went away — reopen */
			fd = -1;
			sleep(1);
			continue;
		}
		if (e.type != EV_KEY)
			continue;

		if (sleeping) {
			if (e.value == 1) {	/* any fresh press wakes */
				write_sysfs_int(FB_BLANK, 0); /* UNBLANK */
				write_sysfs_int(BRIGHTNESS, saved_bl);
				ioctl(fd, EVIOCGRAB, 0);
				sleeping = 0;
			}
			continue;	/* releases/repeats while dark: drop */
		}

		if (e.code == KEY_FN) {
			fn_down = (e.value != 0);
			continue;
		}

		if (fn_down && e.code == KEY_ESC && e.value == 1) {
			saved_bl = read_sysfs_int(BRIGHTNESS, BL_DEFAULT);
			if (saved_bl == 0)
				saved_bl = BL_DEFAULT;
			ioctl(fd, EVIOCGRAB, 1);
			write_sysfs_int(BRIGHTNESS, 0);
			write_sysfs_int(FB_BLANK, 4);	/* FB_BLANK_POWERDOWN */
			sleeping = 1;
			fn_down = 0;	/* releases are ours now */
		}
	}
}
