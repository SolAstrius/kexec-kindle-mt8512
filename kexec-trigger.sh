#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# kexec-trigger.sh - stage a payload with kexec_min.ko and, with --go, jump to it.
#
# Runs on the Kindle as root (busybox sh), from the directory holding the files:
#
#   sh kexec-trigger.sh [--go] [MODULE KERNEL DTB INITRD]
#
# Defaults: kexec_min.ko kernel.bin payload.dtb ramdisk.bin next to this script.
#
# Without --go it loads the module, stages the payload and runs the module's
# self-check, then stops. With --go it also shuts down everything the next
# kernel cannot inherit (WiFi/BT, USB gadget, media pipeline, second CPU) and
# jumps. WiFi goes away, so run --go detached:
#
#   nohup sh kexec-trigger.sh --go > go.out 2>&1 < /dev/null &
#
# Every step is appended to kexec.log next to this script. If the payload
# hangs, the hardware watchdog (left running on purpose) resets the Kindle into
# stock within about 31 s.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
LOG=$HERE/kexec.log
PROC=/proc/kexec_min

MOD=$HERE/kexec_min.ko
KERNEL=$HERE/kernel.bin
DTB=$HERE/payload.dtb
RAMDISK=$HERE/ramdisk.bin
MODE=dryrun

pos=0
for a in "$@"; do
	case "$a" in
	--go)      MODE=go ;;
	--dry-run) MODE=dryrun ;;
	--*)       echo "unknown flag: $a" >&2; exit 2 ;;
	*)
		pos=$((pos + 1))
		case "$pos" in
		1) MOD=$a ;;
		2) KERNEL=$a ;;
		3) DTB=$a ;;
		4) RAMDISK=$a ;;
		*) echo "too many arguments" >&2; exit 2 ;;
		esac
		;;
	esac
done

log() {
	ts="$(date '+%Y-%m-%d %H:%M:%S') up=$(cut -d' ' -f1 /proc/uptime)"
	echo "[$ts] $*"
	echo "[$ts] $*" >>"$LOG" 2>/dev/null
}

abort() {
	log "ABORT: $*"
	exit 1
}

log "==== kexec-trigger ($MODE): $MOD $KERNEL $DTB $RAMDISK"

# ---- preflight -------------------------------------------------------------

if [ ! -d "$PROC" ]; then
	[ -f "$MOD" ] || abort "module not found: $MOD"
	insmod "$MOD" 2>>"$LOG" || abort "insmod failed (built for a different kernel?)"
fi
[ -w "$PROC/ctl" ] || abort "$PROC/ctl missing after insmod"

for f in "$KERNEL" "$DTB" "$RAMDISK"; do
	[ -s "$f" ] || abort "missing or empty: $f"
done

# Falcon (Amazon's hibernation firmware) resumes from p6 when its header is
# armed. Jumping with an armed image risks a resume loop, so require it blank.
# (Count non-NUL bytes; od/hexdump collapse zero runs into a misleading '*'.)
nz=$(dd if=/dev/mmcblk0p6 bs=512 count=9 2>/dev/null | tr -d '\000' | wc -c | tr -d ' ')
[ -n "$nz" ] || abort "could not read /dev/mmcblk0p6"
[ "$nz" = 0 ] || abort "Falcon resume image armed on p6 ($nz non-zero bytes)"

# With a USB host attached the usb power domain never turns off, so the idle
# check below would fail -- after WiFi and USB are already torn down. Check now.
for s in /sys/class/udc/*/state; do
	[ -r "$s" ] || continue
	st=$(cat "$s")
	if [ "$st" != "not attached" ]; then
		[ "$MODE" = go ] && abort "USB is attached ($s: $st); unplug the cable first"
		log "warning: USB is attached ($st); --go will refuse until it is unplugged"
	fi
done

for st in /sys/class/power_supply/*/status; do
	[ -r "$st" ] && log "power: $(basename "$(dirname "$st")") $(cat "$st")"
done

# ---- stage and self-check --------------------------------------------------

cat "$KERNEL"  > "$PROC/kernel" 2>>"$LOG" || abort "staging kernel failed"
cat "$RAMDISK" > "$PROC/initrd" 2>>"$LOG" || abort "staging initrd failed"
cat "$DTB"     > "$PROC/dtb"    2>>"$LOG" || abort "staging dtb failed"
echo dryrun > "$PROC/ctl" 2>>"$LOG" || abort "dryrun failed: $(grep 'last error' "$PROC/status")"
while IFS= read -r line; do log "  $line"; done < "$PROC/status"

if [ "$MODE" != go ]; then
	log "dry run done; pass --go (detached) to jump"
	exit 0
fi

# ---- quiesce ---------------------------------------------------------------

svc_running() { initctl status "$1" 2>/dev/null | grep -q running; }

# UI and power management.
for s in framework powerd lab126_gui cvm x otav webreader; do
	svc_running "$s" && { log "stop $s"; initctl stop "$s" >>"$LOG" 2>&1; }
done
sync
for mp in / /mnt/us; do
	mount -o remount,ro "$mp" 2>/dev/null && log "remounted $mp read-only"
done

# WiFi/BT: stop the daemons that pin the WMT devices, then unload the stack
# leaf first. connsys can DMA into RAM the next kernel owns, so fail closed.
for s in btmanagerd asr_bt_userstore asr_bt_reboot wand wmt; do
	svc_running "$s" && { log "stop $s"; initctl stop "$s" >>"$LOG" 2>&1; }
done
WMT='wlan_drv_gen4m wmt_chrdev_wifi wmt_cdev_bt wmt_drv'
for m in $WMT; do
	grep -q "^$m " /proc/modules && { log "rmmod $m"; rmmod "$m" >>"$LOG" 2>&1; }
done
for m in $WMT; do
	grep -q "^$m " /proc/modules && abort "$m still loaded; reboot to restore WiFi"
done

# The WMT unload leaves the connsys power domain on; power it off properly.
echo conn-off > "$PROC/ctl" 2>>"$LOG" || abort "conn-off failed (see dmesg); reboot to restore WiFi"
log "connsys powered off"

# USB gadget: detach from the controller so it stops DMA and powers down.
for g in /sys/kernel/config/usb_gadget/*; do
	[ -w "$g/UDC" ] && [ -n "$(cat "$g/UDC")" ] && echo "" > "$g/UDC"
done
for m in g_ether usb_f_rndis u_ether; do
	grep -q "^$m " /proc/modules && rmmod "$m" 2>>"$LOG"
done

# Media pipeline: stop mdpd, the userspace client of the MDP/GCE engines, then
# wait until every power domain is off and the GCE clock is idle. Never unbind
# mtk-mdp: its remove path oopses, and panic_on_oops turns that into a panic.
DBG=/sys/kernel/debug
grep -q " $DBG " /proc/mounts || mount -t debugfs none "$DBG" || abort "no debugfs"
svc_running mdpd && { log "stop mdpd"; initctl stop mdpd >/dev/null 2>&1; }
busy_domains() {
	awk 'NR>2 && $1 !~ /^\// && NF>=2 && $2 !~ /^off/ {printf "%s:%s ", $1, $2}' \
		"$DBG/pm_genpd/pm_genpd_summary"
}
gce_enabled() { awk '$1=="infra_gce" {print $2}' "$DBG/clk/clk_summary"; }
i=0
until [ -z "$(busy_domains)" ] && [ "$(gce_enabled)" = 0 ]; do
	i=$((i + 1))
	[ "$i" -le 15 ] || abort "not idle after 15 s (domains: $(busy_domains) gce: $(gce_enabled))"
	sleep 1
done
log "media pipeline idle"

# The module jumps from a single CPU.
echo 0 > /sys/devices/system/cpu/cpu1/online 2>>"$LOG"
[ "$(cat /sys/devices/system/cpu/online)" = 0 ] || abort "cpu1 would not go offline"

# ---- jump ------------------------------------------------------------------

log "jumping"
sync
echo go > "$PROC/ctl" 2>>"$LOG"
log "FAILED: 'go' returned; the jump did not happen"
while IFS= read -r line; do log "  $line"; done < "$PROC/status"
exit 1
