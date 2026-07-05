/* SPDX-License-Identifier: MIT */
/*
 * WHY2025 dropbear trim — appended to localoptions.h by Buildroot
 * (BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE), after the .mk's own NOMMU
 * "#define NON_INETD_MODE 0" hook.
 *
 * ed25519 hostkey + curve25519 kex only. Keeps the dropbearmulti FLAT
 * in-memory image at 514,288 B with the 16 KB stack post-build.sh sets
 * (measured 2026-07-05, dropbear 2026.91 with BR2_PACKAGE_DROPBEAR_
 * CLIENT — dbclient/ssh/scp-out included) — inside the 512 KB order-7
 * bucket with ~10 KB of headroom left. Server-only measured 491,744 B;
 * the stock algo set measures 543,056 B, tipping every per-connection
 * exec into the 1 MB bucket. There is NO room for more options here —
 * re-measure with flthdr after any change. Consequences of the trim:
 * inbound client keys in authorized_keys must be ssh-ed25519 (the
 * RSA/ECDSA verify code is gone), and outbound dbclient can only talk
 * to servers presenting an ed25519 host key.
 */
#define DROPBEAR_RSA 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_DH_GROUP14_SHA256 0
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_X11FWD 0
