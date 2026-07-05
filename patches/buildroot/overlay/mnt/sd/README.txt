No SD card mounted. Insert one and run `mount /mnt/sd` (or reboot; the
card is auto-mounted at boot). FAT32 and exFAT cards work.

Persisted config lives in badge/ on the card (everything optional):
  badge/wifi.conf     SSID='...' and PSK='...'  -> Wi-Fi auto-connect
                      at boot, then a one-shot NTP clock sync
  badge/ssh/authorized_keys
                      ssh-ed25519 public keys allowed to SSH in as root
  badge/dropbear/dropbear_ed25519_host_key
                      persistent SSH host key; create it on the badge:
                      dropbearkey -t ed25519 -f \
                        /mnt/sd/badge/dropbear/dropbear_ed25519_host_key
  badge/profile.sh    sourced by every login shell
  badge/rc.local      sourced as root at the end of boot
