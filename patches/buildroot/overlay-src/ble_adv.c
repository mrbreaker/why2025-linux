// SPDX-License-Identifier: GPL-2.0-only
/*
 * ble_adv — advertise this badge over BLE via esp-hosted-NG SPI HCI.
 *
 * Sibling of ble_scan: binds a raw HCI socket with HCI_CHANNEL_USER
 * (the kernel-init HCIDEVUP path hangs on this hardware), sends HCI
 * Reset, programs LE advertising parameters + data (Flags + Complete
 * Local Name), enables advertising, and then just sits there — the
 * user channel resets the controller when the socket closes, so
 * advertising only lasts as long as the process. Run it in the
 * background: `ble_adv &`. Another badge's `ble_scan` then shows this
 * one as an ADV line carrying the name.
 *
 * The controller is exclusive: ble_adv and ble_scan cannot run at the
 * same time (the second bind fails with EBUSY), and BT is only up
 * ~21 s after boot (patch 0030 defers HCI init).
 *
 * Usage:
 *   ble_adv           advertise the hostname until killed
 *   ble_adv NAME      advertise NAME (max 26 bytes; longer is cut)
 *
 * NOMMU/FLAT: raw write(2) only (no stdio through elf2flt), tiny
 * resident footprint (~16 KB class, pool-backed).
 */
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <poll.h>
#include <sys/socket.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH       31
#endif
#define BTPROTO_HCI        1
#define HCI_CHANNEL_USER   1

#define HCI_COMMAND_PKT    0x01
#define HCI_EVENT_PKT      0x04
#define HCI_EVT_CMD_COMPL  0x0E
#define HCI_EVT_CMD_STATUS 0x0F

#define OP_RESET           0x0C03
#define OP_LE_ADV_PARAMS   0x2006
#define OP_LE_ADV_DATA     0x2008
#define OP_LE_ADV_ENABLE   0x200A

#define ADV_NAME_MAX       26	/* 31 - 3 (Flags AD) - 2 (Name AD hdr) */

struct sockaddr_hci {
	uint16_t hci_family;
	uint16_t hci_dev;
	uint16_t hci_channel;
};

static int sk = -1;
static volatile sig_atomic_t stopping;

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

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

int main(int argc, char **argv)
{
	char name[ADV_NAME_MAX + 1];
	int name_len;

	if (argc > 1) {
		name_len = strlen(argv[1]);
		if (name_len > ADV_NAME_MAX)
			name_len = ADV_NAME_MAX;
		memcpy(name, argv[1], name_len);
	} else {
		memset(name, 0, sizeof name);
		if (gethostname(name, ADV_NAME_MAX) < 0)
			memcpy(name, "why2025", 7);
		name[ADV_NAME_MAX] = 0;
		name_len = strlen(name);
	}

	say("ble_adv: open AF_BLUETOOTH/HCI socket\n");
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
	say("ble_adv: bind hci0 CHANNEL_USER\n");
	int tries = 0;
	while (bind(sk, (struct sockaddr *)&addr, sizeof addr) < 0) {
		if (errno != ENODEV || ++tries > 40) {
			say("bind failed errno="); say_int(errno);
			say(" (-16=EBUSY -> another HCI tool is running)\n");
			return 2;
		}
		usleep(500000);
	}

	say("ble_adv: HCI Reset");
	if (hci_send_cmd(OP_RESET, 0, 0) < 0) return 3;
	if (wait_cmd_complete(OP_RESET, 5000) != 0) return 4;

	/*
	 * LE Set Advertising Parameters: interval min/max 0x00A0/0x00F0
	 * (100/150 ms — 0x00A0 is the spec floor for non-connectable),
	 * type 0x03 ADV_NONCONN_IND (nothing here accepts connections),
	 * own addr public, all 3 channels, no filter.
	 */
	uint8_t ap[] = { 0xA0, 0x00, 0xF0, 0x00, 0x03, 0x00, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00 };
	say("ble_adv: LE Set Adv Params (nonconn, 100-150ms)");
	if (hci_send_cmd(OP_LE_ADV_PARAMS, ap, sizeof ap) < 0) return 5;
	if (wait_cmd_complete(OP_LE_ADV_PARAMS, 3000) != 0) return 5;

	/*
	 * LE Set Advertising Data: fixed 32-byte parameter block =
	 * [significant_len][31 bytes]. Flags AD (LE General Discoverable,
	 * BR/EDR not supported) + Complete Local Name.
	 */
	uint8_t ad[32];
	memset(ad, 0, sizeof ad);
	ad[1] = 0x02; ad[2] = 0x01; ad[3] = 0x06;	/* Flags */
	ad[4] = (uint8_t)(name_len + 1);
	ad[5] = 0x09;					/* Complete Local Name */
	memcpy(&ad[6], name, name_len);
	ad[0] = (uint8_t)(3 + 2 + name_len);		/* significant length */
	say("ble_adv: LE Set Adv Data (name \"");
	write(1, name, name_len);
	say("\")");
	if (hci_send_cmd(OP_LE_ADV_DATA, ad, sizeof ad) < 0) return 6;
	if (wait_cmd_complete(OP_LE_ADV_DATA, 3000) != 0) return 6;

	uint8_t en[] = { 0x01 };
	say("ble_adv: LE Set Advertise Enable");
	if (hci_send_cmd(OP_LE_ADV_ENABLE, en, sizeof en) < 0) return 7;
	if (wait_cmd_complete(OP_LE_ADV_ENABLE, 3000) != 0) return 8;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	say("ble_adv: advertising; kill me to stop (controller resets on exit)\n");

	/*
	 * Park: drain any stray events so the socket buffer can't fill.
	 * Advertising itself needs no host interaction.
	 */
	uint8_t tmp[260];
	while (!stopping)
		hci_read(tmp, sizeof tmp, 1000);

	uint8_t dis[] = { 0x00 };
	say("ble_adv: LE Set Advertise Disable");
	hci_send_cmd(OP_LE_ADV_ENABLE, dis, sizeof dis);
	(void) wait_cmd_complete(OP_LE_ADV_ENABLE, 2000);

	close(sk);
	return 0;
}
