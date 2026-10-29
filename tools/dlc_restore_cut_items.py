"""Restore garage items that shipped in a DLC pack but were commented out of its item list.

Origin of Pain's pack1/items.xml carries RIDER_PART_DLC_RIDING_SHORTS ("DLC shorts", id 302 in the
pants category) inside an XML comment. Everything it needs is in the pack: the model
(DLC1/Scenes/Characters/Outfits/dlc_character_pants_1, collection 2654860915, beside the Team 1/2
jerseys and helmets that are live), the garage icon (char/items/items_pants_dlc.png) and its name in
the localization. Un-commenting the line makes it a normal shop item.

The PC never reads the regionalid attribute these DLC items carry, so the Team 1 / Team 2 promo
gear needs nothing here.

Usage: python dlc_restore_cut_items.py SOURCE_PAK OUTPUT_PAK   (the pack's data/configs.pak)
"""
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                    # noqa: E402
from dlc_unpack import names_of                        # noqa: E402
from dlc_convert import write_pak                      # noqa: E402

ITEMS = "pack1/items.xml"
CUT_ITEMS = ("RIDER_PART_DLC_RIDING_SHORTS",)


def restore(xml):
    """Un-comment each cut item line. Returns (new text, names restored)."""
    done = []
    for name in CUT_ITEMS:
        pattern = re.compile(r'<!--\s*(<item [^>]*name="%s"[^>]*/>)\s*-->' % re.escape(name))
        xml, n = pattern.subn(r"\1", xml)
        if n:
            done.append(name)
    return xml, done


def main(source, output):
    d, files = read_pak(source)
    named = dict(names_of(d, files))
    if ITEMS not in named:
        sys.exit("%s has no %s (not Origin of Pain?)" % (source, ITEMS))
    target = named[ITEMS]
    text = file_data(d, target, decode_xmem=True).decode("latin-1")
    new, done = restore(text)
    if not done:
        print("%s: no commented-out items left to restore" % source)
        return False
    body = new.encode("latin-1")
    entries = []
    for f in files:
        if f is target:
            entries.append((f["hash"], 8, zlib.compress(body, 9), len(body)))
        else:
            entries.append((f["hash"], f["method"], d[f["offset"]:f["offset"] + f["packed"]], f["size"]))
    write_pak(output, entries)
    d2, files2 = read_pak(output)                      # verify: only the item list changed
    assert len(files2) == len(files)
    for f, g in zip(files, files2):
        if f is target:
            assert file_data(d2, g) == body
        else:
            assert d[f["offset"]:f["offset"] + f["packed"]] == d2[g["offset"]:g["offset"] + g["packed"]]
    print("%s: restored %s" % (output, ", ".join(done)))
    return True


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
