// SPDX-License-Identifier: GPL-2.0-only
/*
 * ble_scan — minimal BLE LE scanner over esp-hosted-NG SPI HCI.
 *
 * Binds a raw HCI socket with HCI_CHANNEL_USER to bypass the kernel's
 * controller-init handshake (HCIDEVUP via the kernel-init path hangs on this
 * hardware). Issues HCI Reset, sets active LE scan parameters, enables scan,
 * and prints LE Advertising Reports as they arrive. Disables scan on exit.
 *
 * Usage:
 *   ble_scan              full scan, ~20 s
 *   ble_scan reset        send HCI Reset and exit (round-trip probe)
 */
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH       31
#endif
#define BTPROTO_HCI        1
#define HCI_CHANNEL_USER   1

#define HCI_COMMAND_PKT    0x01
#define HCI_EVENT_PKT      0x04
#define HCI_EVT_CMD_COMPL  0x0E
#define HCI_EVT_CMD_STATUS 0x0F
#define HCI_EVT_LE_META    0x3E
#define HCI_LE_ADV_REPORT  0x02

#define OP_RESET           0x0C03
#define OP_SET_EVENT_MASK  0x0C01
#define OP_LE_SET_EVT_MASK 0x2001
#define OP_LE_SCAN_PARAMS  0x200B
#define OP_LE_SCAN_ENABLE  0x200C

struct sockaddr_hci {
	uint16_t hci_family;
	uint16_t hci_dev;
	uint16_t hci_channel;
};

static int sk = -1;
static int adv_count = 0;
static int evt_count = 0;

static void say(const char *s) { write(1, s, strlen(s)); }

static void hex2(uint8_t v)
{
	const char *h = "0123456789abcdef";
	char b[2] = { h[v >> 4], h[v & 0xf] };
	write(1, b, 2);
}

static void say_int(int v)
{
	char buf[16]; int n = 0; unsigned u;
	if (v < 0) { write(1, "-", 1); u = -v; } else { u = v; }
	if (u == 0) { write(1, "0", 1); return; }
	while (u) { buf[n++] = '0' + u % 10; u /= 10; }
	while (n--) write(1, &buf[n], 1);
}

static int hci_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
	uint8_t pkt[260];
	pkt[0] = HCI_COMMAND_PKT;
	pkt[1] = opcode & 0xff;
	pkt[2] = (opcode >> 8) & 0xff;
	pkt[3] = plen;
	if (plen) memcpy(&pkt[4], params, plen);
	int n = write(sk, pkt, 4 + plen);
	if (n < 0) {
		say("write cmd 0x"); hex2(opcode>>8); hex2(opcode&0xff);
		say(" failed errno="); say_int(errno); say("\n");
		return -1;
	}
	return 0;
}

static int hci_read(uint8_t *buf, int max, int ms)
{
	struct pollfd p = { .fd = sk, .events = POLLIN };
	int r = poll(&p, 1, ms);
	if (r <= 0) return -1;
	return read(sk, buf, max);
}

static int wait_cmd_complete(uint16_t opcode, int ms_total)
{
	uint8_t pkt[260];
	int budget = ms_total;
	while (budget > 0) {
		int n = hci_read(pkt, sizeof pkt, budget);
		if (n < 0) {
			say("timeout waiting for op=0x");
			hex2(opcode>>8); hex2(opcode&0xff); say("\n");
			return -1;
		}
		if (n < 3 || pkt[0] != HCI_EVENT_PKT) { budget -= 50; continue; }
		uint8_t evt = pkt[1];
		if (evt == HCI_EVT_CMD_COMPL && n >= 6) {
			uint16_t op = pkt[4] | (pkt[5] << 8);
			if (op != opcode) { budget -= 50; continue; }
			uint8_t status = (n >= 7) ? pkt[6] : 0xff;
			say(" cmd_complete op=0x");
			hex2(opcode>>8); hex2(opcode&0xff);
			say(" status=0x"); hex2(status); say("\n");
			return status;
		}
		if (evt == HCI_EVT_CMD_STATUS && n >= 6) {
			uint16_t op = pkt[5] | (pkt[6] << 8);
			if (op != opcode) { budget -= 50; continue; }
			uint8_t status = pkt[3];
			say(" cmd_status op=0x");
			hex2(opcode>>8); hex2(opcode&0xff);
			say(" status=0x"); hex2(status); say("\n");
			return status;
		}
		budget -= 50;
	}
	say("budget exhausted op=0x");
	hex2(opcode>>8); hex2(opcode&0xff); say("\n");
	return -1;
}

static void process_le_adv_report(const uint8_t *p, int n)
{
	/* p[0] = subevent (0x02 for adv report), p[1] = num_reports */
	if (n < 2 || p[0] != HCI_LE_ADV_REPORT) {
		say(" le-meta subevent=0x");
		if (n >= 1) hex2(p[0]);
		say(" len="); say_int(n); say("\n");
		return;
	}
	int num = p[1];
	const uint8_t *q = p + 2;
	int rem = n - 2;
	for (int i = 0; i < num && rem >= 9; i++) {
		uint8_t evt_type = q[0];
		uint8_t addr_type = q[1];
		const uint8_t *addr = &q[2];
		uint8_t len = q[8];
		if (rem < 9 + len + 1) return;
		int8_t rssi = (int8_t) q[9 + len];
		(void)evt_type; (void)addr_type;
		say("ADV ");
		for (int j = 5; j >= 0; j--) {
			hex2(addr[j]); if (j) write(1, ":", 1);
		}
		say(" type="); hex2(evt_type);
		say(" at="); hex2(addr_type);
		say(" rssi=");
		if (rssi < 0) { write(1, "-", 1); say_int(-rssi); } else { say_int(rssi); }
		say(" adlen="); say_int(len); say("\n");
		adv_count++;
		q += 9 + len + 1;
		rem -= 9 + len + 1;
	}
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
	int reset_only = (argc > 1);

	say("ble_scan: open AF_BLUETOOTH/HCI socket\n");
	sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sk < 0) {
		say("socket failed errno="); say_int(errno); say("\n");
		return 1;
	}

	struct sockaddr_hci addr = {
		.hci_family = AF_BLUETOOTH,
		.hci_dev = 0,
		.hci_channel = HCI_CHANNEL_USER,
	};
	/* HCI registration is opt-in since kernel patch 0035 (the boot-time
	 * burst wedged cold boots): request it, then wait out ENODEV while
	 * the kernel registers hci0 over the C6 SPI link (a few seconds). */
	int kfd = open("/sys/module/esp32_spi/parameters/bt_enable", O_WRONLY);
	if (kfd >= 0) {
		write(kfd, "1", 1);
		close(kfd);
	}
	say("ble_scan: bind hci0 CHANNEL_USER\n");
	int tries = 0;
	while (bind(sk, (struct sockaddr *)&addr, sizeof addr) < 0) {
		if (errno != ENODEV || ++tries > 40) {
			say("bind failed errno="); say_int(errno);
			say(" (-16=EBUSY -> another HCI tool is running)\n");
			return 2;
		}
		usleep(500000);
	}

	say("ble_scan: HCI Reset");
	if (hci_send_cmd(OP_RESET, 0, 0) < 0) return 3;
	if (wait_cmd_complete(OP_RESET, 5000) != 0) return 4;

	if (reset_only) {
		say("ble_scan: reset OK, exiting (reset-only)\n");
		close(sk);
		return 0;
	}

	/* Drain any pending events. */
	uint8_t tmp[260];
	for (;;) {
		int n = hci_read(tmp, sizeof tmp, 100);
		if (n <= 0) break;
	}

	/* HCI_RESET above wiped event masks back to spec defaults — which
	 * exclude the LE Meta Event. Re-enable, otherwise adv reports never
	 * leave the controller. Mask matches BlueZ's default. */
	uint8_t evm[] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	say("ble_scan: Set Event Mask (incl. LE Meta)");
	if (hci_send_cmd(OP_SET_EVENT_MASK, evm, sizeof evm) < 0) return 5;
	if (wait_cmd_complete(OP_SET_EVENT_MASK, 3000) != 0) return 5;

	/* LE Event Mask: enable adv report, conn complete, conn update, etc. */
	uint8_t levm[] = { 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	say("ble_scan: LE Set Event Mask");
	if (hci_send_cmd(OP_LE_SET_EVT_MASK, levm, sizeof levm) < 0) return 5;
	if (wait_cmd_complete(OP_LE_SET_EVT_MASK, 3000) != 0) return 5;

	/* LE Set Scan Parameters: type=active(1), interval=0x60, window=0x30,
	 * own_addr_type=public(0), filter_policy=accept_all(0).
	 * Active scan TXs scan_req on undirected adverts -> more responses. */
	uint8_t sp[] = { 0x01, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
	say("ble_scan: LE Set Scan Params (active, 60ms/30ms)");
	if (hci_send_cmd(OP_LE_SCAN_PARAMS, sp, sizeof sp) < 0) return 5;
	if (wait_cmd_complete(OP_LE_SCAN_PARAMS, 3000) != 0) return 6;

	/* LE Set Scan Enable: enable=1, filter_dups=0. */
	uint8_t se[] = { 0x01, 0x00 };
	say("ble_scan: LE Set Scan Enable");
	if (hci_send_cmd(OP_LE_SCAN_ENABLE, se, sizeof se) < 0) return 7;
	if (wait_cmd_complete(OP_LE_SCAN_ENABLE, 3000) != 0) return 8;

	say("ble_scan: scanning for 20 seconds...\n");
	uint8_t pkt[260];
	long t_end = now_ms() + 20000;
	for (;;) {
		long left = t_end - now_ms();
		if (left <= 0) break;
		int n = hci_read(pkt, sizeof pkt, left > 500 ? 500 : (int)left);
		if (n <= 0) continue;
		evt_count++;
		if (n >= 3 && pkt[0] == HCI_EVENT_PKT && pkt[1] == HCI_EVT_LE_META) {
			process_le_adv_report(&pkt[3], n - 3);
		} else if (n >= 2 && pkt[0] == HCI_EVENT_PKT) {
			/* Print other events (status, etc.) so we can tell whether the
			 * controller is silent or just chatty in non-LE form. */
			say(" evt=0x"); hex2(pkt[1]); say(" plen="); say_int(pkt[2]); say("\n");
		} else {
			say(" rx pkt[0]=0x"); hex2(pkt[0]); say(" len="); say_int(n); say("\n");
		}
	}

	uint8_t sd[] = { 0x00, 0x00 };
	say("ble_scan: LE Set Scan Disable");
	hci_send_cmd(OP_LE_SCAN_ENABLE, sd, sizeof sd);
	(void) wait_cmd_complete(OP_LE_SCAN_ENABLE, 2000);

	say("ble_scan: total events="); say_int(evt_count);
	say(" advertising_reports="); say_int(adv_count); say("\n");

	close(sk);
	return adv_count > 0 ? 0 : 9;
}
