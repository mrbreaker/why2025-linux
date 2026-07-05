// SPDX-License-Identifier: GPL-2.0-only
/* Minimal hci_up — write(2) only, no stdio. Aims to avoid uClibc FLAT
 * relocation pathologies seen with fprintf/printf in this toolchain.
 * Also doubles as the "enable BT" poke: HCI registration is opt-in
 * since kernel patch 0035 (the boot-time burst wedged cold boots). */
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH    31
#endif
#define BTPROTO_HCI     1
#define HCIDEVUP        _IOW('H', 201, int)

static void say(const char *s) { write(2, s, strlen(s)); }

static void say_int(int v)
{
	char buf[16]; int n = 0; unsigned u;
	if (v < 0) { write(2, "-", 1); u = -v; } else { u = v; }
	if (u == 0) { write(2, "0", 1); return; }
	while (u) { buf[n++] = '0' + u % 10; u /= 10; }
	while (n--) write(2, &buf[n], 1);
}

int main(void)
{
	say("S0 request BT (bt_enable knob), wait for hci0\n");
	int kfd = open("/sys/module/esp32_spi/parameters/bt_enable", O_WRONLY);
	if (kfd >= 0) {
		write(kfd, "1", 1);
		close(kfd);
	}
	int tries = 0;
	while (access("/sys/class/bluetooth/hci0", F_OK) < 0) {
		if (++tries > 40) {
			say("hci0 never appeared\n");
			return 3;
		}
		usleep(500000);
	}

	say("S0 socket\n");
	int s = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (s < 0) {
		say("socket fail errno="); say_int(errno); say("\n");
		return 1;
	}

	say("S1 fd="); say_int(s); say(" issuing HCIDEVUP\n");
	int r = ioctl(s, HCIDEVUP, 0);
	int e = errno;
	say("S2 ret="); say_int(r); say(" errno="); say_int(e); say("\n");

	close(s);
	return (r < 0) ? 2 : 0;
}
