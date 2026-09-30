# kexec-kindle-mt8512

kexec as a loadable module for Kindle devices with MediaTek MT8512 (MT8110/MT8113) processors.
Amazon builds the kernel without `CONFIG_KEXEC`; `kexec_min.ko` adds this option.
The signed boot chain is not modified, and nothing is written to memory.

Tested on Kindle Scribe (2024), firmware 5.17.2, kernel 4.9.77-lab126.
It allows you to boot Amazon's custom kernel into a full userspace with Wi-Fi support and mainline Linux 7.2.8. Other Kindle models with MT8512 processors have not been tested.

Requires a jailbroken and rooted Kindle. Use at your own risk.

The module retrieves the kernel, initramfs, and device tree via /proc/kexec_min/, generates a relocation list for kexec, switches to the identity map, flushes caches, disables the MMU, and executes the transition. It uses only exported
symbols; the rest of the kexec path in the kernel is rewritten based on the 4.9 source code.

Four features of this device prevent kexec from running easily:

- **Falcon.** In the stock firmware, all eMMC I/O goes through Falcon, Amazon's built-in hibernation firmware. It retains the MMIO mappings of the old kernel and hangs after kexec runs. The payload device tree renames the /falcon node so the new kernel never gets into it, and restores the eMMC controller compatibility to mediatek,mt8518-mmc so that control passes to the regular mtk-sd driver.
- **OP-TEE.** Secure World caches shared memory buffers pointing to the old kernel, so all calls from the new kernel fail, and the encrypted user data storage is never unlocked. The module flushes this cache immediately before the transition, just as mainline Linux does during system shutdown since 5.14.
- **WiFi/BT (connsys).** After unloading the Amazon driver, the power is not turned off, and the power-on sequence in the new kernel fails. The module first correctly powers off.
- **Everything else that uses direct memory access.** `kexec-trigger.sh` stops the user interface and Wi-Fi/Bluetooth, disconnects the USB device, waits for the media pipeline to go into standby mode, and shuts down the second processor.

The hardware watchdog timer is never disabled. If the new kernel hangs before its own watchdog driver starts, the Kindle returns to factory settings after approximately 31 seconds. If the hang occurs later, press and hold the power button.

## Build

You'll need Docker and the Amazon firmware source code licensed under the GNU GPL
(`Kindle_src_<version>.tar.gz` from the Amazon source code page):

`./build.sh Kindle_src_5.17.3_4386490030.tar.gz`

This produces `module/kexec_min.ko`, and when built, its Wehrmacht is printed, which should be: `4.9.77-lab126 SMP preempt mod_unload modversions ARMv7 p2v8`.
`kernel/` contains kernel 5.17.2 configuration files and character CRC codes (extracted from the kernel image); other firmware versions require their own files.

## Payload

Extract the standard Kindle boot image and device tree, then build the device tree for the payload:

```bash
ssh kindle 'dd if=/dev/mmcblk0p1 bs=1M count=16 2>/dev/null' > p1.itb
ssh kindle 'cat /sys/firmware/fdt' > live.dtb
tools/fit-extract.py p1.itb stock
tools/make-payload-dtb.py live.dtb stock/ramdisk.bin payload.dtb
```

Both device trees contain your Kindle's identifying information (serial number, MAC addresses, etc.), so do not share them with third parties.

For a different kernel, pass its initramfs to `make-payload-dtb.py` or use
your own device tree with an initrd located at `0x44080000`.

## Launch

```bash
scp module/kexec_min.ko kexec-trigger.sh stock/kernel.bin stock/ramdisk.bin \
payload.dtb kindle:/mnt/us/kexec/
ssh kindle sh /mnt/us/kexec/kexec-trigger.sh
```

This is a dry run; it should complete with `plan: VALID`. To perform the transition, disconnect the USB and run this command on the Kindle in a separate window, as Wi-Fi may be disconnected along the way:

```bash
cd /mnt/us/kexec
nohup sh kexec-trigger.sh --go > go.out 2>&1 < /dev/null &
```

After the transition, the value `kexec_test=1` in `/proc/cmdline` indicates that the payload has been launched; resetting the watchdog returns the device to its initial state. Each execution is recorded in `/mnt/us/kexec/kexec.log`.

Never read `/proc/falcon/*` or disable `mtk-mdp` in the initial state: this may cause the device to crash or change its state.

## License

GPL-2.0. The code for transitioning and transferring control has been ported from the ARM kexec implementation to Linux.
