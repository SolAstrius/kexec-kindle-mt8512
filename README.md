# kexec-kindle-mt8512

kexec as a loadable module for Kindles on the MediaTek MT8512 (MT8110/MT8113).
Amazon builds their kernel without `CONFIG_KEXEC`; `kexec_min.ko` adds it. The
signed boot chain is left alone and nothing is written to storage.

Tested on a Kindle Scribe (2024), firmware 5.17.2, kernel 4.9.77-lab126. It
boots Amazon's own kernel back to full userspace with WiFi, and mainline Linux
7.2.8. Other MT8512 Kindles are untested.

A jailbroken Kindle with a root shell is required. Use at your own risk.

## What it does

The module takes a kernel, initramfs and device tree through
`/proc/kexec_min/`, builds a kexec relocation list, switches to an identity
map, flushes caches, turns the MMU off and jumps. It uses only exported
symbols; the rest of the kernel's kexec path is reimplemented from the 4.9
source.

Four things on this device break a plain kexec:

- **Falcon.** On stock, all eMMC I/O goes through Falcon, Amazon's resident
  hibernation firmware. It keeps the old kernel's MMIO mappings and hangs after
  a kexec. The payload device tree renames the `/falcon` node so the new kernel
  never enters it, and restores the eMMC controller's `mediatek,mt8518-mmc`
  compatible so the regular `mtk-sd` driver takes over.
- **OP-TEE.** The secure world caches shared-memory buffers that point into the
  old kernel, so every call from the new one fails and the encrypted userstore
  never unlocks. The module drains that cache right before the jump, as
  upstream Linux does at shutdown since 5.14.
- **WiFi/BT (connsys).** Unloading Amazon's driver leaves the power domain on,
  and the new kernel's power-on sequence fails from that state. The module
  powers it off properly first.
- **Everything else that does DMA.** `kexec-trigger.sh` stops the UI and
  WiFi/BT, detaches the USB gadget, waits for the media pipeline to go idle
  and takes the second CPU offline.

The hardware watchdog is never disabled. If the new kernel hangs before its
own watchdog driver starts, the Kindle resets into stock within about 31
seconds. A hang after that point needs a long press of the power button.

## Build

You need Docker and Amazon's GPL source release for the firmware
(`Kindle_src_<version>.tar.gz` from Amazon's source code page):

    ./build.sh Kindle_src_5.17.3_4386490030.tar.gz

The result is `module/kexec_min.ko`, and the build prints its vermagic, which
must be `4.9.77-lab126 SMP preempt mod_unload modversions ARMv7 p2v8`.
`kernel/` holds the 5.17.2 kernel's config and symbol CRCs (extracted from the
kernel image); other firmware versions need their own.

## Payload

Pull the stock boot image and the live device tree off the Kindle, then build
the payload device tree:

    ssh kindle 'dd if=/dev/mmcblk0p1 bs=1M count=16 2>/dev/null' > p1.itb
    ssh kindle 'cat /sys/firmware/fdt' > live.dtb
    tools/fit-extract.py p1.itb stock
    tools/make-payload-dtb.py live.dtb stock/ramdisk.bin payload.dtb

Both device trees contain your Kindle's identity store (serial number, MAC
addresses and more), so don't share them.

For a different kernel, pass its initramfs to `make-payload-dtb.py`, or use
its own device tree with the initrd placed at `0x44080000`.

## Run

    scp module/kexec_min.ko kexec-trigger.sh stock/kernel.bin stock/ramdisk.bin \
        payload.dtb kindle:/mnt/us/kexec/
    ssh kindle sh /mnt/us/kexec/kexec-trigger.sh

That is a dry run; it should end with `plan: VALID`. To jump, unplug USB and
run this on the Kindle, detached, because WiFi goes down on the way:

    cd /mnt/us/kexec
    nohup sh kexec-trigger.sh --go > go.out 2>&1 < /dev/null &

After the jump, `kexec_test=1` in `/proc/cmdline` means the payload is
running; a watchdog reset into stock comes back without it. Each run is logged
to `/mnt/us/kexec/kexec.log`.

Never read `/proc/falcon/*` or unbind `mtk-mdp` on stock: both can crash the
device or change its state.

## License

GPL-2.0. The relocation and hand-off code is ported from Linux's ARM kexec
implementation.
