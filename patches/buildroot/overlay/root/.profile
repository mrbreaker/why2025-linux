# ~/.profile — root's login shell (hush), sourced after /etc/profile.
# hush has no `alias` builtin (busybox shell/hush.c: "It does not handle
# select, aliases, tilde expansion") — use functions. Defining one is
# fork-free; the body forks only when invoked.
ll() { ls -la "$@"; }
la() { ls -A "$@"; }
# No clear applet in this busybox; ANSI does it via the printf builtin.
clear() { printf '\033[H\033[J'; }
