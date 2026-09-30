#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Build the kexec payload device tree from the Kindle's live one.

    make-payload-dtb.py LIVE_DTB INITRD OUT_DTB

LIVE_DTB is /sys/firmware/fdt copied from the running Kindle: the tree as
U-Boot patched it, including the idme store. It identifies your device, so
keep it (and the output) private. INITRD is the initramfs you will boot, used
only for its size. Three edits, everything else kept byte for byte:

  /chosen         initrd bounds for INITRD at 0x44080000, and " kexec_test=1"
                  appended to bootargs so a successful jump is recognisable
  /falcon         renamed /falcnx: the kernel looks the node up by name, so
                  Falcon (Amazon's hibernation firmware, which also serves all
                  eMMC I/O on stock and hangs after kexec) is never entered
  /mmc@11230000   compatible falcon_blk -> mediatek,mt8518-mmc, so the
                  kernel's own mtk-sd driver takes the eMMC instead
"""
import struct
import sys

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9

INITRD_START = 0x44080000
INITRD_LIMIT = 0x44600000  # reserved display memory starts here
MARKER = b" kexec_test=1"


def parse(buf):
    """Parse an FDT into nested dicts, keeping node and property order."""
    magic, _, off_struct, off_strings, off_mem, version, last_comp, boot_cpu, \
        size_strings, _ = struct.unpack_from(">10I", buf)
    if magic != FDT_MAGIC:
        raise ValueError("not a device tree blob")
    strings = buf[off_strings:off_strings + size_strings]
    root, stack, p = None, [], off_struct
    while True:
        (tok,) = struct.unpack_from(">I", buf, p)
        p += 4
        if tok == FDT_BEGIN_NODE:
            end = buf.index(b"\0", p)
            node = {"name": buf[p:end].decode(), "props": {}, "children": []}
            p = (end + 4) & ~3
            if stack:
                stack[-1]["children"].append(node)
            root = root or node
            stack.append(node)
        elif tok == FDT_END_NODE:
            stack.pop()
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack_from(">II", buf, p)
            p += 8
            name = strings[nameoff:strings.index(b"\0", nameoff)].decode()
            stack[-1]["props"][name] = buf[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            header = (version, last_comp, boot_cpu)
            return root, buf[off_mem:off_struct], header
        else:
            raise ValueError(f"bad FDT token {tok} at {p - 4:#x}")


def serialize(root, memreserve, header):
    version, last_comp, boot_cpu = header
    strings, stroff, sb = bytearray(), {}, bytearray()

    def string(name):
        if name not in stroff:
            stroff[name] = len(strings)
            strings.extend(name.encode() + b"\0")
        return stroff[name]

    def pad4(b):
        return b + b"\0" * (-len(b) % 4)

    def emit(node):
        sb.extend(struct.pack(">I", FDT_BEGIN_NODE))
        sb.extend(pad4(node["name"].encode() + b"\0"))
        for k, v in node["props"].items():
            sb.extend(struct.pack(">III", FDT_PROP, len(v), string(k)))
            sb.extend(pad4(v))
        for c in node["children"]:
            emit(c)
        sb.extend(struct.pack(">I", FDT_END_NODE))

    emit(root)
    sb.extend(struct.pack(">I", FDT_END))
    off_mem = 40
    off_struct = off_mem + len(memreserve)
    off_strings = off_struct + len(sb)
    total = off_strings + len(strings)
    head = struct.pack(">10I", FDT_MAGIC, total, off_struct, off_strings, off_mem,
                       version, last_comp, boot_cpu, len(strings), len(sb))
    return head + bytes(memreserve) + bytes(sb) + bytes(strings)


def child(node, name):
    return next((c for c in node["children"] if c["name"] == name), None)


def flatten(node, path=""):
    here = f"{path}/{node['name']}" if node["name"] else ""
    out = {f"{here or '/'}:{k}": v for k, v in node["props"].items()}
    for c in node["children"]:
        out.update(flatten(c, here))
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.split("\n\n")[1])
    live_path, initrd_path, out_path = sys.argv[1:]
    live = open(live_path, "rb").read()
    initrd_end = INITRD_START + len(open(initrd_path, "rb").read())
    if initrd_end > INITRD_LIMIT:
        sys.exit(f"initrd too large: ends at {initrd_end:#x}, limit {INITRD_LIMIT:#x}")

    root, memreserve, header = parse(live)

    chosen = child(root, "chosen")
    for key, value in (("linux,initrd-start", INITRD_START),
                       ("linux,initrd-end", initrd_end)):
        cells = len(chosen["props"][key]) // 4
        chosen["props"][key] = struct.pack(f">{cells}I", *([0] * (cells - 1) + [value]))
    bootargs = chosen["props"]["bootargs"]
    if MARKER.strip() not in bootargs:
        chosen["props"]["bootargs"] = bootargs.rstrip(b"\0") + MARKER + b"\0"

    falcon = child(root, "falcon")
    if falcon is None:
        sys.exit("no /falcon node: not the tree this tool expects")
    falcon["name"] = "falcnx"

    mmc = child(root, "mmc@11230000")
    if mmc is None or mmc["props"].get("compatible") != b"falcon_blk\0":
        sys.exit("/mmc@11230000 is not falcon_blk: not the tree this tool expects")
    mmc["props"]["compatible"] = b"mediatek,mt8518-mmc\0"

    out = serialize(root, memreserve, header)

    # Check that only the intended properties and the one node name changed.
    before = flatten(parse(live)[0])
    after = flatten(parse(out)[0])
    renamed = {k.replace("/falcon:", "/falcnx:", 1): v for k, v in before.items()}
    changed = sorted(k for k in set(renamed) | set(after) if renamed.get(k) != after.get(k))
    expected = ["/chosen:bootargs", "/chosen:linux,initrd-end",
                "/chosen:linux,initrd-start", "/mmc@11230000:compatible"]
    if [k for k in changed if k not in expected]:
        sys.exit(f"unexpected changes: {changed}")

    open(out_path, "wb").write(out)
    print(f"{out_path}: initrd {INITRD_START:#x}..{initrd_end:#x}, "
          f"Falcon off, native eMMC ({len(out)} bytes)")


if __name__ == "__main__":
    main()
