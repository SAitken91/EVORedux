"""Renumber the Origin of Pain (pack 1) events so they stop colliding with the PC game's events.

Trials Evolution Gold uses event ids 201-207 for its Trials HD section (pack0/events.xml, part="1").
Pack 1 uses 201-208 too, so on PC its events showed the HD events' tracks (e.g. Theory of
Acceleration = PC event 205), launching them gave a black screen, and its EVENT_NAME_XLAST_20x
strings replaced the HD event names. Pack 1 events become 211-218 (pack 2 uses 301-306, which is
free). Changed, in the converted pack (run after dlc_convert.py):
  configs.pak  pack1/events.xml, pack1/awards.xml, localization/*/pack1.xml (EVENT_NAME_XLAST_<id>)
  data_misc.pak  images/Event_<id>.png.tex (renamed: new name hash + name table)
Rewritten XML is stored zlib-compressed (method 8) like the PC game's files; images keep their bytes.
Running it again changes nothing.

Usage: python tools/dlc_events.py [<converted pack dir>]
       python tools/dlc_events.py --unlock [<dlc_pc root>]
--unlock opens every DLC event (both packs) on the world map: license="0" and unlockMedals="0" in
pack<N>/events.xml (the originals need up to the level-5 license and 50/100 medals).
"""
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                  # noqa: E402
from dlc_unpack import names_of, NAME_TABLE           # noqa: E402
from dlc_convert import write_pak                     # noqa: E402
from xmem import xmem_decompress, is_xmem             # noqa: E402

EVENT_MAP = {old: old + 10 for old in range(201, 209)}


def name_hash(name):
    """The engine's pak name hash (0x888c20): uppercase, '\\' -> '/', seeded with the length."""
    k, acc = len(name), 0
    for c in name.encode("latin-1"):
        if 97 <= c <= 122:
            c -= 32
        if c == 0x5C:
            c = 0x2F
        acc = (acc + c * k) & 0xFFFFFFFF
        k = ((k >> 16) + (k & 0xFFFF) * 18000) & 0xFFFFFFFF
    return acc


def entry_bytes(d, f):
    b = file_data(d, f)
    return xmem_decompress(b) if f["method"] == 4 and is_xmem(b) else b


def renumber_xml(name, xml):
    if name.startswith("localization/"):
        pattern = rb'(name="EVENT_NAME_XLAST_)(\d+)(")'
    else:                                                  # events.xml / awards.xml
        pattern = rb'(<event id=")(\d+)(")'
    new = re.sub(pattern, lambda m: m.group(1) + str(EVENT_MAP.get(int(m.group(2)), int(m.group(2)))).encode()
                 + m.group(3), xml)
    if new != xml and parses(xml):      # some game XML has duplicate attributes; only compare like with like
        ET.fromstring(new.decode("utf-8-sig").encode("utf-8"))
    return new


def parses(xml):
    try:
        ET.fromstring(xml.decode("utf-8-sig").encode("utf-8"))
        return True
    except ET.ParseError:
        return False


def rewrite_pak(path, edit):
    """edit(name, entry, data_fn) -> None (keep) or (name, method, packed, size)."""
    d, files = read_pak(path)
    named = names_of(d, files)
    table = [f for f in files if f["hash"] == NAME_TABLE]
    out, names, changed = [], [], 0
    for name, f in named:
        raw = d[f["offset"]:f["offset"] + f["packed"]]
        res = edit(name, f, lambda: entry_bytes(d, f))
        if res is None:
            out.append((f["hash"], f["method"], raw, f["size"]))
            names.append(name)
        else:
            new_name, method, packed, size = res
            out.append((name_hash(new_name) if new_name != name else f["hash"], method, packed, size))
            names.append(new_name)
            changed += 1
    if not changed:
        return 0
    if table:      # name table last and stored, as in the original paks
        blob = struct.pack("<I", len(names)) + b"".join(struct.pack("<H", len(n)) + n.encode("latin-1")
                                                          for n in names)
        out.append((NAME_TABLE, 0, blob, len(blob)))
    write_pak(path, out)
    return changed


def main(pack_dir):
    def configs_edit(name, f, data):
        if name in ("pack1/events.xml", "pack1/awards.xml") or (name.startswith("localization/")
                                                                  and name.endswith("/pack1.xml")):
            xml = data()
            new = renumber_xml(name, xml)
            if new != xml:
                return name, 8, zlib.compress(new, 9), len(new)
        return None

    data_dir = os.path.join(pack_dir, "data")
    n = rewrite_pak(os.path.join(data_dir, "configs.pak"), configs_edit)
    print(f"configs.pak: {n} entries renumbered")

    # Images keep their packed bytes; only the name (hash) changes.
    misc = os.path.join(data_dir, "data_misc.pak")
    d, _files = read_pak(misc)

    def misc_edit(name, f, data):
        m = re.fullmatch(r"images/Event_(\d+)\.png\.tex", name)
        if m and int(m.group(1)) in EVENT_MAP:
            raw = d[f["offset"]:f["offset"] + f["packed"]]
            return f"images/Event_{EVENT_MAP[int(m.group(1))]}.png.tex", f["method"], raw, f["size"]
        return None
    n = rewrite_pak(misc, misc_edit)
    print(f"data_misc.pak: {n} event images renamed")


def unlock(root):
    for folder, pack in (("OriginofPain", 1), ("RidersofDoom", 2)):
        def edit(name, f, data):
            if name != f"pack{pack}/events.xml":
                return None
            xml = data()
            new = re.sub(rb"<event [^>]*>", lambda tag: re.sub(rb'\b(license|unlockMedals)="\d+"', rb'\1="0"',
                                                                tag.group(0)), xml)
            return None if new == xml else (name, 8, zlib.compress(new, 9), len(new))
        n = rewrite_pak(os.path.join(root, folder, "data", "configs.pak"), edit)
        print(f"pack{pack}: events {'unlocked' if n else 'already unlocked'}")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--unlock"]
    if "--unlock" in sys.argv:
        unlock(args[0] if args else r"O:\trialsevo\dlc_pc")
    else:
        main(args[0] if args else r"O:\trialsevo\dlc_pc\OriginofPain")
