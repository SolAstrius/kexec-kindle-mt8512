#!/usr/bin/env python3
"""Dependency-free U-Boot FIT (.itb) lister/extractor.

    fit_extract.py kernel-p1.itb            # list the tree (data blobs summarised)
    fit_extract.py kernel-p1.itb OUTDIR     # also write each /images/* data blob to OUTDIR/<name>.bin

Supports embedded `data` and external `data-offset`/`data-position` + `data-size`.
Verifies each image's hash (sha1/sha256/md5/crc32) where present.
"""
import hashlib, json, os, struct, sys, zlib

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def parse(buf):
    magic, total, off_struct, off_strings, _, version, _, _, size_strings, size_struct = struct.unpack_from(">10I", buf)
    assert magic == 0xD00DFEED, "not an FDT"
    strings = buf[off_strings:off_strings + size_strings]
    root, stack, p = None, [], off_struct
    while True:
        (tok,) = struct.unpack_from(">I", buf, p); p += 4
        if tok == FDT_BEGIN_NODE:
            end = buf.index(b"\0", p); name = buf[p:end].decode(); p = (end + 4) & ~3
            node = {"name": name, "props": {}, "children": []}
            (stack[-1]["children"].append(node) if stack else None); root = root or node; stack.append(node)
        elif tok == FDT_END_NODE:
            stack.pop()
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack_from(">II", buf, p); p += 8
            pname = strings[nameoff:strings.index(b"\0", nameoff)].decode()
            stack[-1]["props"][pname] = buf[p:p + ln]; p = (p + ln + 3) & ~3
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            return root, total
        else:
            raise ValueError(f"bad token {tok} at {p - 4:#x}")


def fmt(v, key=None):
    if key in ("load", "entry", "timestamp") and len(v) == 4:
        return f"<{struct.unpack('>I', v)[0]:#x}>"
    if len(v) > 64:
        return f"<{len(v)} bytes>"
    if v and v[-1:] == b"\0" and all(32 <= c < 127 or c == 0 for c in v[:-1]) and v[:1] != b"\0":
        return " | ".join(s.decode() for s in v[:-1].split(b"\0"))
    if len(v) % 4 == 0 and v:
        return "<" + " ".join(f"{x:#x}" for x in struct.unpack(f">{len(v)//4}I", v)) + ">"
    return v.hex()


def dump(node, ind=0):
    print("  " * ind + (node["name"] or "/") + " {")
    for k, v in node["props"].items():
        print("  " * (ind + 1) + f"{k} = {fmt(v, k)}")
    for c in node["children"]:
        dump(c, ind + 1)
    print("  " * ind + "}")


def child(node, name):
    return next((c for c in node["children"] if c["name"] == name), None)


def u32(v):
    return struct.unpack(">I", v)[0]


def image_data(buf, total, img):
    pr = img["props"]
    if "data" in pr:
        return pr["data"]
    size = u32(pr["data-size"])
    if "data-position" in pr:
        o = u32(pr["data-position"])
    else:
        o = ((total + 3) & ~3) + u32(pr["data-offset"])
    return buf[o:o + size]


def check_hash(img, data):
    out = []
    for h in img["children"]:
        if not h["name"].startswith("hash"):
            continue
        algo = h["props"]["algo"].rstrip(b"\0").decode(); want = h["props"]["value"]
        if algo == "crc32":
            got = struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)
        else:
            got = hashlib.new(algo, data).digest()
        out.append(f"{algo} {'OK' if got == want else 'MISMATCH'}")
    return ", ".join(out) or "no hash"


def main():
    buf = open(sys.argv[1], "rb").read()
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    root, total = parse(buf)
    dump(root)
    images = child(root, "images")
    manifest = {}
    print("\n== images")
    for img in images["children"]:
        data = image_data(buf, total, img)
        meta = {k: fmt(v, k) for k, v in img["props"].items() if k != "data"}
        meta["size"] = len(data); meta["hash_check"] = check_hash(img, data)
        manifest[img["name"]] = meta
        print(f"{img['name']}: {len(data)} bytes, {meta['hash_check']}, "
              f"type={meta.get('type')} comp={meta.get('compression')} load={meta.get('load')} entry={meta.get('entry')}")
        if outdir:
            os.makedirs(outdir, exist_ok=True)
            open(os.path.join(outdir, img["name"] + ".bin"), "wb").write(data)
    if outdir:
        json.dump(manifest, open(os.path.join(outdir, "manifest.json"), "w"), indent=2)


if __name__ == "__main__":
    main()
